"""Background processes for the stage, with logs and a registry another `gtut` can stop."""
from __future__ import annotations

import json
import os
import signal
import subprocess
import time
from pathlib import Path


class Procs:
    def __init__(self, stage_dir: Path):
        self.dir = stage_dir
        self.logs = stage_dir / 'logs'
        self.logs.mkdir(parents=True, exist_ok=True)
        self.registry = stage_dir / 'procs.json'
        self.live: dict[str, subprocess.Popen] = {}

    def _load(self) -> dict:
        try:
            return json.loads(self.registry.read_text())
        except (OSError, ValueError):
            return {}

    def _save(self, data: dict):
        tmp = self.registry.with_suffix('.tmp')
        tmp.write_text(json.dumps(data, indent=1))
        tmp.replace(self.registry)

    def start(self, name: str, argv: list[str], env: dict | None = None, cwd: Path | None = None) -> subprocess.Popen:
        self.stop(name)
        log = open(self.logs / f'{name}.log', 'ab', buffering=0)
        log.write(f'\n=== {time.strftime("%H:%M:%S")} {" ".join(map(str, argv))}\n'.encode())
        p = subprocess.Popen([str(a) for a in argv], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             env=env, cwd=cwd, start_new_session=True)
        self.live[name] = p
        data = self._load()
        data[name] = dict(pid=p.pid, argv=[str(a) for a in argv], started=time.time())
        self._save(data)
        return p

    def alive(self, name: str) -> bool:
        info = self._load().get(name)
        if not info:
            return False
        try:
            os.kill(info['pid'], 0)
        except OSError:
            return False
        p = self.live.get(name)
        return p is None or p.poll() is None

    def stop(self, name: str, timeout: float = 5.0):
        data = self._load()
        info = data.pop(name, None)
        self._save(data)
        p = self.live.pop(name, None)
        pid = p.pid if p else (info or {}).get('pid')
        if not pid:
            return
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            return
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                os.kill(pid, 0)
            except OSError:
                break
            if p is not None and p.poll() is not None:
                break
            time.sleep(0.05)
        else:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        if p is not None:
            try:
                p.wait(1)
            except subprocess.TimeoutExpired:
                pass

    def stop_all(self, order: list[str] | None = None):
        names = list(self._load().keys())
        order = [n for n in (order or []) if n in names] + [n for n in reversed(names) if n not in (order or [])]
        for name in order:
            self.stop(name)

    def tail(self, name: str, lines: int = 20) -> str:
        try:
            return ''.join((self.logs / f'{name}.log').read_text(errors='replace').splitlines(True)[-lines:])
        except OSError:
            return ''


def wait_until(predicate, timeout: float, interval: float = 0.1, what: str = 'condition'):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except Exception as exc:  # predicates probe things that are still starting
            last = exc
        time.sleep(interval)
    raise TimeoutError(f'timed out after {timeout:.0f}s waiting for {what} (last: {last!r})')
