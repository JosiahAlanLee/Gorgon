"""Takes: the stage's picture (x11grab) and programme sound (JACK) in one ffmpeg."""
from __future__ import annotations

import subprocess
import time
from pathlib import Path

from . import audio
from .display import x_env
from .procs import wait_until


def video_args(encoder: str, quality: int, intermediate: bool = True) -> list[str]:
    if encoder == 'h264_nvenc':
        args = ['-c:v', 'h264_nvenc', '-preset', 'p4', '-tune', 'hq', '-rc', 'constqp', '-qp', str(quality)]
        return args + (['-pix_fmt', 'yuv444p', '-profile:v', 'high444p'] if intermediate else ['-pix_fmt', 'yuv420p'])
    if encoder == 'libx264':
        args = ['-c:v', 'libx264', '-preset', 'veryfast', '-crf', str(quality)]
        return args + (['-pix_fmt', 'yuv444p'] if intermediate else ['-pix_fmt', 'yuv420p'])
    raise ValueError(f'unknown encoder {encoder!r}')


class Take:
    """One continuous recording of a screen rectangle plus three audio channels:
    programme left, programme right, and the slate click."""

    def __init__(self, cfg, path: Path, rect, slate: audio.Slate | None):
        self.cfg = cfg
        self.path = Path(path)
        self.rect = [int(v) for v in rect]
        self.slate = slate
        self.proc: subprocess.Popen | None = None
        self.client = f'gtut_rec_{int(time.time() * 1000) % 100000}'
        self.started = None
        self.log = self.path.with_suffix('.ffmpeg.log')

    def start(self):
        c = self.cfg['capture']
        x, y, w, h = self.rect
        w -= w % 2
        h -= h % 2
        self.path.parent.mkdir(parents=True, exist_ok=True)
        argv = ['ffmpeg', '-hide_banner', '-y', '-loglevel', 'info',
                '-thread_queue_size', '1024', '-f', 'x11grab', '-framerate', str(c['fps']),
                '-video_size', f'{w}x{h}', '-draw_mouse', '1' if c['draw_mouse'] else '0',
                '-i', f'{self.cfg.display}+{x},{y}',
                '-thread_queue_size', '1024', '-f', 'jack', '-channels', '3', '-i', self.client,
                *video_args(c['encoder'], c['quality']),
                '-c:a', 'pcm_s24le', str(self.path)]
        env = audio.jack_env(self.cfg, x_env(self.cfg))
        self.proc = subprocess.Popen(audio.wrap(self.cfg, argv), stdin=subprocess.PIPE,
                                     stdout=open(self.log, 'wb'), stderr=subprocess.STDOUT, env=env)
        # ffmpeg's JACK input registers <client>:input_1..3; route programme and slate into it.
        wait_until(lambda: f'{self.client}:input_3' in audio.ports(output=False), 15,
                   what='the recorder JACK ports')
        try:
            left, right = audio.resolve_record_ports(self.cfg)
            audio.connect(left, f'{self.client}:input_1')
            audio.connect(right, f'{self.client}:input_2')
        except RuntimeError as exc:
            # No programme yet (e.g. a Studio-only shot): record silence, say so in the log.
            with open(self.log, 'ab') as f:
                f.write(f'\n[gtut] programme not connected: {exc}\n'.encode())
        if self.slate:
            audio.connect(self.slate.name, f'{self.client}:input_3')
        self.started = time.time()
        # Wait until frames are actually being written before anything happens on stage.
        wait_until(lambda: self.path.exists() and self.path.stat().st_size > 4096, 15, what='first frames')
        return self

    def stop(self):
        if not self.proc:
            return
        try:
            self.proc.stdin.write(b'q')
            self.proc.stdin.flush()
            self.proc.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait(5)
        code = self.proc.returncode
        self.proc = None
        if code not in (0, 255):
            raise RuntimeError(f'ffmpeg exited {code}; see {self.log}')

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
