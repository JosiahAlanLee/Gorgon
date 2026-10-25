"""A filmed terminal: xterm with a neutral prompt, typed into by the Hand.

Output is logged so actions can wait for it. Nothing personal reaches the screen:
the prompt is bare, history is off, and `show_safe()` filters the session token
and home directory out of anything a command prints; `hydrapy` and `pip` are
wrapped in it, so they can be typed plainly on camera.
"""
from __future__ import annotations

import os
import re
import shlex
import time
from pathlib import Path

from . import audio, display
from .procs import Procs, wait_until

RCFILE = r"""
PS1='\[\e[38;5;179m\]\W \$\[\e[0m\] '
unset HISTFILE
export HISTSIZE=0
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PYTHONUNBUFFERED=1
show_safe() { "$@" 2>&1 | sed -u -E -e 's/#[A-Za-z0-9_-]{20,}/#<session token>/g' -e "s#$HOME#~#g"; }
# The commands filmed most print paths (pip's "Obtaining file:///home/...", hydrapy's FFmpeg paths) or Studio's
# token: filter them in place, so what is typed on camera is the plain command a viewer would type.
hydrapy() { show_safe command hydrapy "$@"; }
pip() { show_safe command pip "$@"; }
clear
"""


class Terminal:
    def __init__(self, cfg, procs: Procs, hand, cwd: Path | None = None, extra_env: dict | None = None):
        self.cfg, self.procs, self.hand = cfg, procs, hand
        self.log = cfg.stage_dir() / 'terminal.log'
        if self.log.exists():
            self.log.unlink()
        rc = cfg.stage_dir() / 'terminal.rc'
        rc.write_text(RCFILE)
        st = cfg['stage']
        x, y, w, h = st['windows']['terminal']
        env = audio.jack_env(cfg, display.x_env(cfg))
        env.update(extra_env or {})
        env['TERM'] = 'xterm-256color'
        argv = ['xterm', '-fa', st['terminal_font'], '-fs', st['terminal_font_size'],
                '-bg', '#101014', '-fg', '#d8d4cc', '-cr', '#ffc733', '-bc', '+sb', '-b', '24',
                '-geometry', f'+{x}+{y}', '-T', 'gtut-terminal', '-l', '-lf', self.log,
                '-e', 'bash', '--noprofile', '--rcfile', rc, '-i']
        self.proc = procs.start('terminal', argv, env=env, cwd=cwd or cfg.work)
        self.window = display.wait_window(cfg, timeout=20, name='gtut-terminal')
        display.place(cfg, self.window, (x, y, w, h))
        time.sleep(0.6)

    def text(self) -> str:
        try:
            raw = self.log.read_text(errors='replace')
        except OSError:
            return ''
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\r', '', raw)

    def run(self, cmd: str, wait_for: str | None = None, timeout: float = 600, cps: float = 16,
            enter_pause: float = 0.35):
        self.type(cmd, cps, enter_pause)
        if wait_for:
            self.wait(wait_for, timeout)

    def type(self, cmd: str, cps: float = 16, enter_pause: float = 0.35):
        """Type a command and press Enter; output from here on is what wait() searches."""
        display.focus(self.cfg, self.window)
        self.since = len(self.text())
        self.hand.type(cmd, cps=cps, window=self.window)
        time.sleep(enter_pause)
        self.hand.key('Return', window=self.window)

    def wait(self, wait_for: str, timeout: float = 600):
        """Wait for output of the last typed command to match a regex."""
        pattern, since = re.compile(wait_for, re.M), getattr(self, 'since', 0)
        wait_until(lambda: pattern.search(self.text()[since:]), timeout, interval=0.25,
                   what=f'terminal output matching {wait_for!r}')

    def quiet(self, cmd: str):
        """Run without typing on camera (setup between takes): pasted, then cleared."""
        display.focus(self.cfg, self.window)
        self.hand.key('ctrl+u', window=self.window)
        self.hand._xdo(['type', '--window', self.window, '--delay', 1, cmd + '; clear\n'])

    def interrupt(self):
        self.hand.key('ctrl+c', window=self.window)

    def clear(self):
        self.hand.key('ctrl+l', window=self.window)
