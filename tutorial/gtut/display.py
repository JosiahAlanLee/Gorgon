"""The stage's X display, its window manager, and window placement."""
from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path

from .procs import Procs, wait_until

OPENBOX_RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <focus><focusNew>yes</focusNew><followMouse>no</followMouse></focus>
  <placement><policy>Smart</policy></placement>
  <desktops><number>1</number></desktops>
  <applications>
    <application class="*"><decor>no</decor><focus>yes</focus></application>
  </applications>
</openbox_config>
"""

NVIDIA_XORG = """Section "ServerLayout"
    Identifier "gtut"
    Screen 0 "gtut-screen"
EndSection
Section "Device"
    Identifier "gtut-gpu"
    Driver "nvidia"
    BusID "{bus_id}"
    Option "AllowEmptyInitialConfiguration" "true"
    Option "UseDisplayDevice" "none"
EndSection
Section "Screen"
    Identifier "gtut-screen"
    Device "gtut-gpu"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Virtual {w} {h}
    EndSubSection
EndSection
"""


def x_env(cfg, base: dict | None = None) -> dict:
    env = dict(base or os.environ)
    env['DISPLAY'] = cfg.display
    env.pop('WAYLAND_DISPLAY', None)
    return env


def display_ready(cfg) -> bool:
    return subprocess.run(['xdpyinfo', '-display', cfg.display], capture_output=True).returncode == 0


def nvidia_bus_id() -> str:
    out = subprocess.run(['nvidia-smi', '--query-gpu=pci.bus_id', '--format=csv,noheader'],
                         capture_output=True, text=True, check=True).stdout.strip().splitlines()[0]
    # 00000000:01:00.0 -> PCI:1:0:0
    dom, bus, rest = out.split(':')
    dev, fn = rest.split('.')
    return f'PCI:{int(bus, 16)}:{int(dev, 16)}:{int(fn, 16)}'


def start_display(cfg, procs: Procs):
    mode = cfg['stage']['display']
    w, h = cfg['stage']['screen']
    if display_ready(cfg):
        raise RuntimeError(f'display {cfg.display} is already in use; pick another stage.display_number')
    if mode in ('xvfb', 'xvfb+virtualgl'):
        procs.start('display', ['Xvfb', cfg.display, '-screen', '0', f'{w}x{h}x24', '-nolisten', 'tcp',
                                '+extension', 'GLX', '+extension', 'RANDR', '-dpi', '96', '-noreset'])
    elif mode == 'nvidia-xorg':
        conf = cfg.stage_dir() / 'xorg-nvidia.conf'
        conf.write_text(NVIDIA_XORG.format(bus_id=nvidia_bus_id(), w=w, h=h))
        # A second X server on the GPU needs root. sudo -n fails instead of prompting
        # inside a capture; run `sudo -v` first (CLAUDE.md says when to ask).
        procs.start('display', ['sudo', '-n', 'Xorg', cfg.display, '-config', str(conf), '-noreset',
                                '-nolisten', 'tcp', '-logfile', str(procs.logs / 'Xorg.log')])
    else:
        raise ValueError(f'unknown stage.display {mode!r}')
    wait_until(lambda: display_ready(cfg), 15, what=f'X display {cfg.display}')
    if shutil.which('xsetroot'):
        subprocess.run(['xsetroot', '-solid', '#101014'], env=x_env(cfg), capture_output=True)
    if cfg['stage']['wm'] == 'openbox':
        rc = cfg.stage_dir() / 'openbox-rc.xml'
        rc.write_text(OPENBOX_RC)
        procs.start('wm', ['openbox', '--config-file', str(rc)], env=x_env(cfg))
        time.sleep(0.5)


def gl_prefix(cfg) -> list[str]:
    """Command prefix for an OpenGL app (Rack) so it renders on the GPU when the mode allows."""
    if cfg['stage']['display'] == 'xvfb+virtualgl':
        if not shutil.which('vglrun'):
            raise RuntimeError('stage.display is xvfb+virtualgl but vglrun is not installed')
        return ['vglrun', '-d', 'egl']
    return []


# Windows ---------------------------------------------------------------

def xdo(cfg, *args, check=True) -> str:
    r = subprocess.run(['xdotool', *map(str, args)], env=x_env(cfg), capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f'xdotool {" ".join(map(str, args))}: {r.stderr.strip()}')
    return r.stdout.strip()


def find_windows(cfg, *, pid: int | None = None, name: str | None = None, cls: str | None = None) -> list[int]:
    args = ['search', '--onlyvisible']
    if pid is not None:
        args += ['--pid', pid]
    if cls is not None:
        args += ['--class', cls]
    if name is not None:
        args += ['--name', name]
    elif pid is None and cls is None:
        args += ['--name', '.']
    out = xdo(cfg, *args, check=False)
    return [int(x) for x in out.split()] if out else []


def wait_window(cfg, timeout: float = 30, **query) -> int:
    return wait_until(lambda: (find_windows(cfg, **query) or [None])[-1], timeout, interval=0.25,
                      what=f'window {query}')


def place(cfg, wid: int, rect):
    x, y, w, h = rect
    xdo(cfg, 'windowsize', wid, w, h)
    xdo(cfg, 'windowmove', wid, x, y)


def geometry(cfg, wid: int) -> tuple[int, int, int, int]:
    out = xdo(cfg, 'getwindowgeometry', '--shell', wid)
    vals = dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
    return int(vals['X']), int(vals['Y']), int(vals['WIDTH']), int(vals['HEIGHT'])


def focus(cfg, wid: int):
    xdo(cfg, 'windowactivate', '--sync', wid, check=False)
    xdo(cfg, 'windowfocus', '--sync', wid, check=False)


def raise_window(cfg, wid: int):
    xdo(cfg, 'windowraise', wid, check=False)


def screenshot(cfg, path: Path, rect=None) -> Path:
    """Grab the stage (or a rectangle of it) as PNG with ffmpeg; independent of any app."""
    w, h = cfg['stage']['screen']
    x, y = 0, 0
    if rect:
        x, y, w, h = rect
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'x11grab', '-video_size', f'{w}x{h}', '-draw_mouse', '1',
                    '-i', f'{cfg.display}+{x},{y}', '-frames:v', '1', str(path)], check=True, env=x_env(cfg))
    return path
