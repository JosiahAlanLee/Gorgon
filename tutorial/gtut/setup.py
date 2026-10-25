"""One-time and per-change preparation: HydraPy at the right commit, plugins built
and installed into the stage's Rack, the catalog, patches and offline renders."""
from __future__ import annotations

import importlib.util
import inspect
import json
import subprocess
import sys
from pathlib import Path

from . import rack as rackmod
from .procs import Procs


def run(argv, **kw):
    print('$', ' '.join(map(str, argv)), flush=True)
    return subprocess.run([str(a) for a in argv], check=True, **kw)


def hydrapy_sha(cfg) -> str:
    ref = cfg['hydrapy_ref']
    if ref == 'pin':
        return (cfg.path('gorgon') / 'hydrapy.pin').read_text().strip()
    src = cfg.path('hydrapy')
    run(['git', '-C', src, 'fetch', 'origin'])
    return subprocess.run(['git', '-C', src, 'rev-parse', 'origin/main' if ref == 'main' else ref],
                          capture_output=True, text=True, check=True).stdout.strip()


def hydrapy_tree(cfg) -> Path:
    """A worktree of the user's HydraPy checkout at the commit being filmed; their checkout is untouched."""
    return cfg.work / 'hydrapy'


def prepare_hydrapy(cfg):
    sha = hydrapy_sha(cfg)
    tree = hydrapy_tree(cfg)
    src = cfg.path('hydrapy')
    # The commit may not be in the checkout yet (a pin moved by CI); fetching it is harmless.
    subprocess.run(['git', '-C', str(src), 'fetch', '--quiet', 'origin', sha], check=False)
    if tree.exists():
        run(['git', '-C', tree, 'checkout', '--quiet', '--detach', sha])
    else:
        run(['git', '-C', src, 'worktree', 'add', '--detach', tree, sha])
    run([sys.executable, '-m', 'pip', 'install', '-q', '-e', f'{tree}[gpu,video,jack]'])
    (cfg.work / 'hydrapy.sha').write_text(sha + '\n')
    return sha


def plugin_dir(cfg, spec: str) -> Path:
    repo, _, sub = spec.partition(':')
    return cfg.path(repo) / sub


def build_plugins(cfg, clean: bool = False) -> list[Path]:
    sdk = cfg.path('rack_sdk')
    packages = []
    for spec in cfg['rack']['plugins']:
        d = plugin_dir(cfg, spec)
        if clean:
            run(['make', '-C', d, f'RACK_DIR={sdk}', 'clean'])
        run(['make', '-C', d, f'RACK_DIR={sdk}', '-j8', 'dist'])
        found = sorted((d / 'dist').glob('*.vcvplugin'), key=lambda p: p.stat().st_mtime)
        if not found:
            raise RuntimeError(f'{d}: make dist produced no .vcvplugin')
        packages.append(found[-1])
    rackmod.install_plugins(cfg, packages)
    versions = {}
    for spec in cfg['rack']['plugins']:
        d = plugin_dir(cfg, spec)
        head = subprocess.run(['git', '-C', d, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
        versions[spec] = dict(version=json.loads((d / 'plugin.json').read_text())['version'], commit=head)
    (cfg.work / 'plugins.json').write_text(json.dumps(versions, indent=1))
    return packages


def catalog(cfg, stage) -> Path:
    stage.up({'rack'})
    out = cfg.work / 'catalog.json'
    if out.exists():
        out.unlink()
    stage.rack.probe.send('catalog', path=str(out), wait=False)
    from .procs import wait_until
    wait_until(lambda: out.exists(), 60, what='the catalog')
    return out


def build_patches(cfg) -> list[Path]:
    from . import vcvpatch
    cat = vcvpatch.load_catalog(cfg.work / 'catalog.json')
    harvested = cfg.root / 'patches' / 'harvested'
    built = []
    for src in sorted((cfg.root / 'patches').glob('[0-9]*.py')):
        spec = importlib.util.spec_from_file_location(src.stem.replace('-', '_'), src)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        # Builders that take a third argument get the config (paths, e.g. DSP_Emulation's example patches).
        args = (cat, harvested, cfg) if len(inspect.signature(mod.build).parameters) > 2 else (cat, harvested)
        for name, patch in mod.build(*args).items():
            path = patch.save(cfg.work / 'patches' / f'{name}.vcv')
            path.with_suffix('.cables.txt').write_text('\n'.join(patch.explain()) + '\n')
            built.append(path)
    return built


def renders(cfg, chapters) -> list[Path]:
    """Offline HydraPy renders used by render shots and card backgrounds."""
    tree = hydrapy_tree(cfg) if hydrapy_tree(cfg).exists() else cfg.path('hydrapy')
    hydrapy = Path(sys.executable).parent / 'hydrapy'
    jobs = {}
    for ch in chapters:
        for s in ch.shots:
            r = s.raw.get('render') if s.kind == 'render' else None
            if r:
                jobs[r.get('name', s.key)] = r
            if s.kind == 'card' and s.raw.get('video'):
                jobs.setdefault(s.raw['video'], dict(patch=str(cfg.root / 'cards' / f"{s.raw['video']}.py"), seconds=12))
    out = []
    w, h = cfg['output']['size']
    for name, r in jobs.items():
        dst = cfg.work / 'renders' / f'{name}.mp4'
        if dst.exists():
            out.append(dst)
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        patch = Path(r['patch'])
        patch = patch if patch.is_absolute() else tree / patch
        argv = [hydrapy, 'render', patch, '-o', dst, '--gpu', '--seconds', r.get('seconds', 10),
                '--size', f"{r.get('size', [w, h])[0]}x{r.get('size', [w, h])[1]}", '--fps', cfg['output']['fps'],
                '--encoder', 'nvenc' if 'nvenc' in cfg['output']['encoder'] else 'auto', *r.get('args', [])]
        env = dict(__import__('os').environ)
        env.pop('DISPLAY', None)  # EGL on the GPU
        run(argv, cwd=tree, env=env)
        out.append(dst)
    return out
