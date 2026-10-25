"""Is this machine ready to film? Each check says PASS, WARN or FAIL and how to fix it."""
from __future__ import annotations

import importlib
import json
import os
import shutil
import socket
import subprocess
import sys
from pathlib import Path

RESULTS = []


def check(name, ok, detail='', fix='', warn=False):
    level = 'PASS' if ok else ('WARN' if warn else 'FAIL')
    RESULTS.append(dict(check=name, level=level, detail=detail, fix=fix if not ok else ''))


def sh(*args, env=None) -> subprocess.CompletedProcess:
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, env=env)


def run(cfg) -> list[dict]:
    RESULTS.clear()
    st = cfg['stage']
    # tools
    tools = {'Xvfb': 'xvfb', 'xdotool': 'xdotool', 'xdpyinfo': 'x11-utils', 'xsetroot': 'x11-xserver-utils',
             'xterm': 'xterm', 'ffmpeg': 'ffmpeg', 'ffprobe': 'ffmpeg', 'jackd': 'jackd2 (or jackd1)',
             'rsvg-convert': 'librsvg2-bin', 'fc-match': 'fontconfig', 'git': 'git', 'make': 'build-essential'}
    if st['wm'] == 'openbox':
        tools['openbox'] = 'openbox'
    if st['display'] == 'xvfb+virtualgl':
        tools['vglrun'] = 'VirtualGL 3.x (virtualgl.org; the .deb from its GitHub releases)'
    if cfg['audio']['server'] == 'pipewire':
        tools['pw-jack'] = 'pipewire-jack'
        tools['pipewire'] = 'pipewire-bin'
    for exe, pkg in tools.items():
        check(f'tool {exe}', bool(shutil.which(exe)), fix=f'sudo apt install {pkg}')
    check('tool tesseract (privacy OCR)', bool(shutil.which('tesseract')), warn=True,
          fix='sudo apt install tesseract-ocr; without it QC cannot scan frames for the session token')
    # ffmpeg capabilities
    if shutil.which('ffmpeg'):
        devs = sh('ffmpeg', '-hide_banner', '-devices').stdout
        check('ffmpeg x11grab input', 'x11grab' in devs, fix='install a full ffmpeg (Ubuntu ffmpeg has it)')
        check('ffmpeg jack input', ' jack ' in devs, fix='install an ffmpeg built with --enable-libjack (Ubuntu ffmpeg is)')
        for enc in {cfg['capture']['encoder'], cfg['output']['encoder']}:
            r = sh('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc=s=1280x720:d=0.5', '-c:v', enc, '-f', 'null', '-')
            check(f'encoder {enc}', r.returncode == 0, r.stderr.strip()[-200:],
                  fix='NVENC needs the NVIDIA driver and an ffmpeg with nvenc; or set encoder: libx264')
    # GPU
    smi = sh('nvidia-smi', '--query-gpu=name,driver_version,memory.used,memory.total', '--format=csv,noheader') \
        if shutil.which('nvidia-smi') else None
    check('NVIDIA GPU', bool(smi and smi.returncode == 0), smi.stdout.strip() if smi else 'nvidia-smi missing',
          fix='the 4090 driver; without it use libx264 and the cpu Studio backend', warn=True)
    # JACK isolation. Where PipeWire's libjack is the system default, a client ignores JACK_DEFAULT_SERVER and
    # joins the desktop session. jackd mode links the real JACK libraries into work/stage/libjack (audio.libjack_dir);
    # either way, a client started exactly as the stage starts them must fail to reach a server that isn't
    # the stage's (a random name; for pipewire an empty runtime directory). The probe registers no ports.
    from . import audio
    if cfg['audio']['server'] == 'jackd':
        lib = audio.real_libjack()
        check('real libjack installed', bool(lib), lib or 'only a PipeWire libjack is installed',
              fix='sudo apt install libjack0 (or libjack-jackd2-0); or set audio.server: pipewire')
    try:
        iso = audio.isolation_probe(cfg)
    except Exception as exc:
        iso = dict(error=f'{type(exc).__name__}: {exc}')
    if 'error' in iso:
        check('JACK clients cannot reach other servers', False, iso['error'], fix='pip install JACK-Client; see the detail')
    else:
        detail = f"loads {iso.get('libjack')}" + (f"; reached a server with {iso['ports']} ports, e.g. {iso['sample']}"
                                                    if iso['connected'] else '; refused a server that is not the stage\'s')
        check('JACK clients cannot reach other servers', not iso['connected'], detail,
              fix='set audio.libjack to a real libjack.so.0 (jackd mode), or audio.server: pipewire with pw-jack installed')
    # Python side
    for mod, why in [('yaml', 'pip install pyyaml'), ('numpy', 'pip install numpy'), ('cv2', 'pip install opencv-python-headless'),
                     ('PIL', 'pip install pillow'), ('jack', 'pip install JACK-Client'), ('playwright', 'pip install playwright'),
                     ('hydrapy', 'gtut setup hydrapy')]:
        try:
            importlib.import_module(mod)
            check(f'python {mod}', True)
        except Exception as exc:
            check(f'python {mod}', False, str(exc), fix=why)
    eng = cfg['narration']['engine']
    if eng == 'kokoro':
        for mod in ('kokoro', 'soundfile'):
            try:
                importlib.import_module(mod)
                check(f'python {mod} (narration)', True)
            except Exception as exc:
                check(f'python {mod} (narration)', False, str(exc), fix='pip install kokoro soundfile; sudo apt install espeak-ng')
        check('espeak-ng (kokoro fallback)', bool(shutil.which('espeak-ng')), warn=True, fix='sudo apt install espeak-ng')
    if cfg['narration']['align'] in ('auto', 'whisper'):
        try:
            importlib.import_module('faster_whisper')
            check('python faster_whisper (cue alignment)', True)
        except Exception:
            check('python faster_whisper (cue alignment)', False, warn=cfg['narration']['align'] == 'auto',
                  fix='pip install faster-whisper; otherwise cues are placed by character position')
    # Playwright's Chromium
    try:
        from playwright.sync_api import sync_playwright
        with sync_playwright() as p:
            exe = str(cfg.path('chromium')) if cfg.path('chromium') else p.chromium.executable_path
        check('Chromium for the controller', Path(exe).exists(), exe, fix='python -m playwright install chromium')
    except Exception as exc:
        check('Chromium for the controller', False, str(exc), fix='python -m playwright install chromium')
    # HydraPy: commit and GPU renderer through EGL
    from .setup import hydrapy_sha, hydrapy_tree
    try:
        want = hydrapy_sha(cfg)
        tree = hydrapy_tree(cfg)
        have = sh('git', '-C', tree, 'rev-parse', 'HEAD').stdout.strip() if tree.exists() else ''
        check('HydraPy worktree at the filmed commit', have == want, f'want {want[:10]}, have {have[:10] or "none"}',
              fix='gtut setup hydrapy')
    except Exception as exc:
        check('HydraPy worktree at the filmed commit', False, str(exc), fix='check paths.gorgon and paths.hydrapy')
    if cfg['studio']['backend'] == 'gl':
        env = dict(os.environ)
        env.pop('DISPLAY', None)
        code = ("from hydrapy.backend_gl import _create_context as c; x=c(); "
                "print(x.info['GL_RENDERER'])")
        r = sh(sys.executable, '-c', code, env=env)
        renderer = r.stdout.strip() or r.stderr.strip()[-200:]
        check('Studio GL renders on the GPU (EGL)', r.returncode == 0 and 'llvmpipe' not in renderer.lower(),
              renderer, fix='install libegl1 and the NVIDIA EGL vendor library; or studio.backend: cpu')
    # Rack
    rack = cfg.path('rack')
    check('Rack executable', bool(rack and rack.exists()), str(rack), fix='paths.rack: the Rack *Free* 2.x binary')
    log = cfg.stage_dir() / 'rack-user' / 'log.txt'
    if log.exists():
        head = log.read_text(errors='replace')[:2000]
        check('Rack is Rack Free', 'Free' in head, head.splitlines()[0] if head else '',
              fix='use VCV Rack Free: an unsigned Rack Pro processes no audio (Gorgon README)')
    else:
        check('Rack is Rack Free', False, 'not started yet', warn=True, fix='run gtut catalog once, then preflight again')
    sdk = cfg.path('rack_sdk')
    check('Rack SDK', bool(sdk and (sdk / 'plugin.mk').exists()), str(sdk), fix='paths.rack_sdk: unzip Rack-SDK-2.x-lin-x64')
    for spec in cfg['rack']['plugins']:
        from .setup import plugin_dir
        d = plugin_dir(cfg, spec)
        check(f'plugin source {spec}', (d / 'plugin.json').exists(), str(d), fix='check paths in config.yaml')
    plugins = cfg.stage_dir() / 'rack-user' / 'plugins-lin-x64'
    have = {p.name.split('-')[0] for p in plugins.iterdir()} if plugins.exists() else set()
    for slug in ('GorgonBridge', 'OtisLab', 'VastishLab', 'SpoonBenderLab', 'SeanceLab', 'GorgonTutorialProbe'):
        check(f'installed in stage Rack: {slug}', slug in have, fix='gtut setup plugins')
    check('catalog', (cfg.work / 'catalog.json').exists(), warn=True, fix='gtut catalog')
    # ports and displays
    with socket.socket() as s:
        free = s.connect_ex(('127.0.0.1', int(cfg['studio']['port']))) != 0
    check(f"Studio port {cfg['studio']['port']} free", free, fix='another Studio is on it; change studio.port')
    disp_free = sh('xdpyinfo', '-display', cfg.display).returncode != 0 if shutil.which('xdpyinfo') else True
    check(f'display {cfg.display} free', disp_free, fix='gtut stage down, or change stage.display_number')
    real = Path.home() / '.hydrapy' / 'studio.json'
    check('a real Studio is not disturbed', True, f'stage uses {cfg.stage_dir() / "studio.json"}'
          + (f'; your own Studio is advertised at {real}' if real.exists() else ''))
    # disk
    du = shutil.disk_usage(cfg.work)
    check('disk space for takes', du.free > 60e9, f'{du.free / 1e9:.0f} GB free in {cfg.work}', warn=True,
          fix='4:4:4 takes run ~1-2 GB per minute of stage')
    return RESULTS


def report(results) -> str:
    width = max(len(r['check']) for r in results)
    lines = []
    for r in results:
        line = f"{r['level']:4}  {r['check']:<{width}}  {r['detail']}"
        if r['fix']:
            line += f"\n      -> {r['fix']}"
        lines.append(line)
    fails = sum(r['level'] == 'FAIL' for r in results)
    warns = sum(r['level'] == 'WARN' for r in results)
    lines.append(f'\n{fails} failed, {warns} warnings')
    return '\n'.join(lines)
