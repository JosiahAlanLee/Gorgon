"""From takes and narration to chapter videos and the full tutorial.

Per shot: find the slate, align picture and programme sound, cut to the narration,
frame the picture (crop/zoom keyframes over the stage windows), draw overlays,
mix the voice over the programme. Then chapters are joined, loudness-normalised,
captioned, and the whole tutorial gets chapter markers.
"""
from __future__ import annotations

import json
import math
import shutil
import subprocess
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

from .capture import video_args
from .narrate import RATE
from .script import Chapter, Shot

FLASH_LUMA = 180  # the terminal's reverse video is light grey, not white


class ComposeError(RuntimeError):
    pass


# helpers -------------------------------------------------------------------

def probe_media(path: Path) -> dict:
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(path)],
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def read_audio(path: Path, channels: int | None = None, rate: int = RATE) -> np.ndarray:
    """All channels as float32 [n, ch]."""
    info = probe_media(path)
    a = next((s for s in info['streams'] if s['codec_type'] == 'audio'), None)
    if not a:
        return np.zeros((0, channels or 2), dtype=np.float32)
    ch = channels or int(a['channels'])
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:a:0', '-ac', str(ch), '-ar', str(rate),
                          '-f', 'f32le', '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<f4').reshape(-1, ch).copy()


def write_wav(path: Path, audio: np.ndarray, rate: int = RATE):
    audio = np.atleast_2d(audio)
    if audio.shape[0] < audio.shape[1]:
        audio = audio.T
    pcm = (np.clip(audio, -1, 1) * 32767).astype('<i2')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 's16le', '-ar', str(rate), '-ac', str(pcm.shape[1]),
                    '-i', '-', str(path)], input=pcm.tobytes(), check=True)


class Frames:
    """Decode a video's frames as RGB arrays, sequentially, from a start time."""

    def __init__(self, path: Path, start: float = 0.0, size=None):
        info = probe_media(path)
        v = next(s for s in info['streams'] if s['codec_type'] == 'video')
        self.w, self.h = (size or (int(v['width']), int(v['height'])))
        num, den = map(int, v['avg_frame_rate'].split('/'))
        self.fps = num / den if den else 30.0
        args = ['ffmpeg', '-v', 'error', '-ss', f'{max(0, start):.3f}', '-i', str(path)]
        if size:
            args += ['-vf', f'scale={self.w}:{self.h}:flags=area']
        args += ['-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        self.index = -1
        self.frame = None

    def read(self):
        n = self.w * self.h * 3
        buf = self.proc.stdout.read(n)
        if len(buf) < n:
            return None
        self.index += 1
        self.frame = np.frombuffer(buf, dtype=np.uint8).reshape(self.h, self.w, 3)
        return self.frame

    def at(self, index: int):
        """Frame by index (forward only; repeats the last when the take runs out)."""
        while self.index < index:
            if self.read() is None:
                break
        return self.frame

    def close(self):
        self.proc.stdout.close()
        self.proc.kill()
        self.proc.wait()


class Writer:
    def __init__(self, path: Path, size, fps: float, encoder: str, quality: int, intermediate=True):
        self.size = size
        args = ['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{size[0]}x{size[1]}',
                '-r', str(fps), '-i', '-', *video_args(encoder, quality, intermediate), str(path)]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE)
        self.count = 0

    def write(self, rgb: np.ndarray):
        self.proc.stdin.write(np.ascontiguousarray(rgb).tobytes())
        self.count += 1

    def close(self):
        self.proc.stdin.close()
        if self.proc.wait() != 0:
            raise ComposeError('encoder failed')


def smooth(t: float) -> float:
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def font(cfg, size: int, mono: bool = False):
    name = cfg['stage']['terminal_font'] if mono else cfg['output']['font']
    for candidate in (name, name.replace(' ', '') + '.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    found = subprocess.run(['fc-match', '-f', '%{file}', name], capture_output=True, text=True).stdout
    return ImageFont.truetype(found, size) if found else ImageFont.load_default()


# slate ------------------------------------------------------------------------

def find_slate(take: Path, hint: float | None = None, search: float = 4.0) -> dict:
    """Video time of the first white frame and audio time of the click, in the take.

    hint: when the runner fired the slate (take.json slate_wall), seconds into the take.
    """
    lo = max(0.0, (hint or 0.0) - 1.5)
    hi = (hint or 0.0) + search if hint is not None else 12.0
    fr = Frames(take, lo, size=(160, 90))
    tv = None
    base = None
    while fr.index < (hi - lo) * fr.fps:
        f = fr.read()
        if f is None:
            break
        cells = f.reshape(3, 30, 4, 40, 3).mean(axis=(1, 3, 4))  # 3x4 grid of cell means
        if base is None:
            base = cells
        if ((cells > FLASH_LUMA) & ((cells - base) > 110)).mean() > 0.5:
            tv = lo + fr.index / fr.fps
            break
    fps = fr.fps
    fr.close()
    a = read_audio(take, channels=3)
    ta = None
    if a.shape[0]:
        seg = a[int(lo * RATE): int(hi * RATE), 2]
        hits = np.flatnonzero(np.abs(seg) > 0.5)
        ta = lo + hits[0] / RATE if len(hits) else None
    return dict(video=tv, audio=ta, fps=fps, offset=(tv - ta) if tv is not None and ta is not None else 0.0,
                found=tv is not None and ta is not None)


# framing ------------------------------------------------------------------------

def frame_track(cfg, shot: Shot, take: dict, cue_time) -> list[tuple[float, list[float]]]:
    """Keyframes (time in take seconds, crop rect in take pixels), 16:9."""
    ox, oy = take['rect'][0], take['rect'][1]
    out_w, out_h = cfg['output']['size']
    aspect = out_w / out_h
    windows = cfg['stage']['windows']
    frames = shot.frames or [dict(window=take['windows'][0])]
    track = []
    for f in frames:
        t = cue_time(f.get('at'))
        wx, wy, ww, wh = windows[f.get('window', take['windows'][0])]
        if f.get('rect'):
            rx, ry, rw, rh = f['rect']
            if all(0 <= v <= 1 for v in f['rect']):  # fractions of the window
                rx, ry, rw, rh = rx * ww, ry * wh, rw * ww, rh * wh
        else:
            rx, ry, rw, rh = 0, 0, ww, wh
        zoom = float(f.get('zoom', 1.0))
        cx, cy = wx + rx + rw / 2 - ox, wy + ry + rh / 2 - oy
        rw, rh = rw / zoom, rh / zoom
        if rw / rh > aspect:
            rh = rw / aspect
        else:
            rw = rh * aspect
        track.append((t, [cx - rw / 2, cy - rh / 2, rw, rh], float(f.get('ease', 0.7))))
    track.sort(key=lambda k: k[0])
    return track


def rect_at(track, t: float) -> list[float]:
    cur = track[0][1]
    for i, (kt, rect, ease) in enumerate(track):
        if t < kt:
            break
        prev = track[i - 1][1] if i else rect
        k = smooth((t - kt) / ease) if ease > 0 else 1.0
        cur = [p + (r - p) * k for p, r in zip(prev, rect)]
    return cur


def crop(frame: np.ndarray, rect, size) -> np.ndarray:
    h, w = frame.shape[:2]
    x, y, rw, rh = rect
    # keep inside the take; letterbox with the stage background if the rect spills over
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    x1, y1 = int(math.ceil(x + rw)), int(math.ceil(y + rh))
    pad = max(0, -x0, -y0, x1 - w, y1 - h)
    if pad:
        frame = cv2.copyMakeBorder(frame, pad, pad, pad, pad, cv2.BORDER_CONSTANT, value=(16, 16, 20))
        x0, y0, x1, y1 = x0 + pad, y0 + pad, x1 + pad, y1 + pad
    sub = frame[y0:y1, x0:x1]
    interp = cv2.INTER_AREA if sub.shape[1] > size[0] else cv2.INTER_LANCZOS4
    return cv2.resize(sub, tuple(size), interpolation=interp)


# overlays --------------------------------------------------------------------------

class _Vars(dict):
    def __missing__(self, key):
        return '{' + key + '}'


_VARS = {}


def text_vars(cfg) -> dict:
    """{hydrapy_sha}, {gorgon_sha}, {plugin:<spec>} for overlay and card text."""
    if id(cfg) not in _VARS:
        v = _Vars()
        sha = cfg.work / 'hydrapy.sha'
        if sha.exists():
            v['hydrapy_sha'] = sha.read_text().strip()[:10]
        try:
            v['gorgon_sha'] = subprocess.run(['git', '-C', str(cfg.path('gorgon')), 'rev-parse', '--short=10', 'HEAD'],
                                             capture_output=True, text=True).stdout.strip()
        except Exception:
            pass
        plugins = cfg.work / 'plugins.json'
        if plugins.exists():
            for spec, info in json.loads(plugins.read_text()).items():
                v[f'plugin:{spec}'] = f"{info['version']} ({info['commit']})"
        _VARS[id(cfg)] = v
    return _VARS[id(cfg)]


def fill(cfg, text: str) -> str:
    return str(text).format_map(text_vars(cfg)) if '{' in str(text) else str(text)


def draw_overlays(cfg, rgb: np.ndarray, active: list[tuple[dict, float]]) -> np.ndarray:
    if not active:
        return rgb
    img = Image.fromarray(rgb).convert('RGBA')
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    W, H = img.size
    s = H / 1080
    for o, alpha in active:
        a = int(255 * alpha)
        kind = o.get('kind', 'label')
        text = fill(cfg, o.get('text', ''))
        o = dict(o, sub=fill(cfg, o.get('sub', '')))
        if kind == 'lower_third':
            f1 = font(cfg, int(44 * s))
            f2 = font(cfg, int(28 * s))
            x, y = int(72 * s), int(H - 200 * s)
            d.rectangle([x - 24 * s, y - 20 * s, x + max(d.textlength(text, f1), d.textlength(o.get('sub', ''), f2)) + 28 * s,
                         y + 110 * s], fill=(10, 10, 14, int(a * 0.82)))
            d.rectangle([x - 24 * s, y - 20 * s, x - 18 * s, y + 110 * s], fill=(255, 199, 51, a))
            d.text((x, y), text, font=f1, fill=(240, 236, 228, a))
            if o.get('sub'):
                d.text((x, y + 58 * s), o['sub'], font=f2, fill=(200, 196, 188, a))
        elif kind == 'keycap':
            f = font(cfg, int(40 * s), mono=True)
            tw = d.textlength(text, f)
            x, y = W - tw - 120 * s, int(80 * s)
            d.rounded_rectangle([x - 22 * s, y - 14 * s, x + tw + 22 * s, y + 58 * s], radius=10 * s,
                                fill=(24, 24, 30, int(a * 0.9)), outline=(255, 199, 51, a), width=max(2, int(3 * s)))
            d.text((x, y), text, font=f, fill=(255, 230, 160, a))
        elif kind == 'caption':
            f = font(cfg, int(34 * s))
            tw = d.textlength(text, f)
            x, y = (W - tw) / 2, H - 96 * s
            d.rectangle([x - 18 * s, y - 10 * s, x + tw + 18 * s, y + 48 * s], fill=(0, 0, 0, int(a * 0.7)))
            d.text((x, y), text, font=f, fill=(255, 255, 255, a))
        else:  # label: at a position given as fractions of the frame
            f = font(cfg, int(32 * s))
            px, py = o.get('pos', [0.06, 0.08])
            x, y = px * W, py * H
            tw = d.textlength(text, f)
            d.rounded_rectangle([x - 14 * s, y - 8 * s, x + tw + 14 * s, y + 44 * s], radius=6 * s,
                                fill=(10, 10, 14, int(a * 0.85)))
            d.text((x, y), text, font=f, fill=(255, 217, 102, a))
    return np.asarray(Image.alpha_composite(img, layer).convert('RGB'))


def overlay_windows(shot: Shot, cue_time, end: float) -> list[tuple[dict, float, float]]:
    out = []
    for o in shot.overlays:
        start = cue_time(o.get('at', 0))
        until = o.get('until')
        if isinstance(until, str) and until.startswith('+'):
            stop = start + float(until[1:])
        elif until is None:
            stop = start + 4.0
        else:
            stop = cue_time(until)
        out.append((o, start, min(stop, end)))
    return out


def active_overlays(windows, t: float, fade: float = 0.25):
    act = []
    for o, a, b in windows:
        if a <= t <= b:
            act.append((o, min(1.0, (t - a) / fade, (b - t) / fade)))
    return act


# shots ------------------------------------------------------------------------------

class Composer:
    def __init__(self, cfg):
        self.cfg = cfg
        self.out = cfg.work / 'segments'
        self.out.mkdir(parents=True, exist_ok=True)
        self.size = tuple(cfg['output']['size'])
        self.fps = float(cfg['output']['fps'])

    def _writer(self, path):
        return Writer(path, self.size, self.fps, self.cfg['capture']['encoder'], 14)

    def narration(self, shot: Shot) -> tuple[dict, np.ndarray]:
        d = self.cfg.work / 'narration' / shot.key
        meta = json.loads((d / 'meta.json').read_text())
        voice = read_audio(d / 'voice.wav', channels=1)[:, 0]
        return meta, voice

    def shot(self, shot: Shot) -> Path:
        if shot.kind == 'live':
            return self.live(shot)
        if shot.kind == 'card':
            return self.card(shot)
        if shot.kind == 'still':
            return self.still(shot)
        if shot.kind == 'render':
            return self.render(shot)
        raise ComposeError(f'{shot.key}: unknown kind {shot.kind}')

    def _mix(self, shot: Shot, voice: np.ndarray, voice_at: float, programme: np.ndarray | None,
             length: float, path: Path):
        n = int(length * RATE)
        mix = np.zeros((n, 2), dtype=np.float32)
        level = shot.raw.get('mix', {})
        if programme is not None and len(programme):
            g = 10 ** (float(level.get('programme', -14 if len(voice) > 1 else -3)) / 20)
            m = min(n, len(programme))
            mix[:m] += programme[:m, :2] * g
        if len(voice) > 1:
            i = int(voice_at * RATE)
            m = max(0, min(n - i, len(voice)))
            mix[i:i + m] += (voice[:m] * 10 ** (float(level.get('voice', 0)) / 20))[:, None]
        fade = int(0.05 * RATE)
        if n > 2 * fade:
            ramp = np.linspace(0, 1, fade, dtype=np.float32)[:, None]
            mix[:fade] *= ramp
            mix[-fade:] *= ramp[::-1]
        write_wav(path, mix)

    def live(self, shot: Shot) -> Path:
        take_dir = self.cfg.work / 'takes' / shot.key / 'latest'
        take_path = take_dir / 'take.mkv'
        if not take_path.exists():
            raise ComposeError(f'{shot.key}: no take; run gtut shoot {shot.chapter}:{shot.id}')
        take = json.loads((take_dir / 'take.json').read_text())
        meta, voice = self.narration(shot)
        if take['narration'].get('digest') != meta.get('digest'):
            raise ComposeError(f'{shot.key}: the narration changed after this take was shot, so its cues no '
                               f'longer match the actions; reshoot it (gtut shoot {shot.chapter}:{shot.id})')
        slate = find_slate(take_path, take.get('slate_wall'))
        (take_dir / 'slate.json').write_text(json.dumps(slate, indent=1))
        if slate['video'] is None:
            raise ComposeError(f'{shot.key}: no slate flash in the take; check {take_dir}')
        start = slate['video'] + 0.25                   # cut after the flash
        voice_at = take['lead'] - 0.25                  # first word, in segment seconds
        marks = {e['mark']: e['t'] for e in take['log'] if 'mark' in e}

        def cue_time(name):                             # segment seconds
            if name in (None, 0):
                return 0.0
            if name in meta['cues']:
                return voice_at + meta['cues'][name]
            if name in marks:
                return marks[name] - 0.25
            raise ComposeError(f'{shot.key}: unknown cue or mark {name!r}')

        length = voice_at + meta['duration'] + shot.hold
        acts = [e for e in take['log'] if 'action' in e]
        if acts:
            length = max(length, acts[-1]['end'] - 0.25 + shot.hold)
        # speed ranges: segment-time spans played faster (never under the voice)
        spans = []
        for sp in shot.speed:
            a, b = cue_time(sp['from']), cue_time(sp['to'])
            if a < voice_at + meta['duration'] and b > voice_at and len(voice) > 1:
                raise ComposeError(f'{shot.key}: speed range {sp} overlaps narration')
            spans.append((a, b, float(sp.get('factor', 8))))

        spans.sort()

        def source_time(t_out: float) -> float:
            src = out = 0.0
            for a, b, k in spans:
                if t_out < out + (a - src):
                    return src + (t_out - out)
                out, src = out + (a - src), a
                d = (b - a) / k
                if t_out < out + d:
                    return a + (t_out - out) * k
                out, src = out + d, b
            return src + (t_out - out)

        out_len = length - sum((b - a) - (b - a) / k for a, b, k in spans)
        track = frame_track(self.cfg, shot, take, cue_time)
        over = overlay_windows(shot, cue_time, length)
        src = Frames(take_path, start)
        seg = self.out / f'{shot.key}.mkv'
        w = self._writer(seg)
        for i in range(int(round(out_len * self.fps))):
            t_src = source_time(i / self.fps)
            f = src.at(int(round(t_src * src.fps)))
            if f is None:
                break
            rgb = crop(f, rect_at(track, t_src), self.size)
            w.write(draw_overlays(self.cfg, rgb, active_overlays(over, t_src)))
        src.close()
        w.close()
        # programme sound, aligned by the slate (ch1/ch2), cut like the picture
        prog = read_audio(take_path, channels=3)[:, :2]
        a0 = int(round((start - slate['offset']) * RATE)) if slate['found'] else int(start * RATE)
        prog = prog[max(0, a0):]
        if spans:
            pieces, cursor = [], 0.0
            for a, b, k in spans:
                pieces.append(prog[int(cursor * RATE):int(a * RATE)])
                pieces.append(np.zeros((int((b - a) / k * RATE), 2), dtype=np.float32))  # sped spans are silent
                cursor = b
            pieces.append(prog[int(cursor * RATE):])
            prog = np.concatenate(pieces)
        self._mix(shot, voice, voice_at, prog, out_len, seg.with_suffix('.wav'))
        self._captions(shot, meta, voice_at, seg)
        return seg

    def _captions(self, shot: Shot, meta: dict, voice_at: float, seg: Path):
        caps = [dict(start=c['start'] + voice_at, end=c['end'] + voice_at, text=c['text']) for c in meta['captions']]
        seg.with_suffix('.captions.json').write_text(json.dumps(caps, indent=1))

    def _background(self, shot: Shot) -> np.ndarray:
        bg = shot.raw.get('background')
        if bg:
            p = Path(bg) if Path(bg).is_absolute() else self.cfg.root / bg
            frames = Frames(self._rasterise(p), 0, size=self.size)
            f = frames.read()
            frames.close()
            return f
        return np.full((self.size[1], self.size[0], 3), (16, 16, 20), dtype=np.uint8)

    def _rasterise(self, p: Path) -> Path:
        if p.suffix.lower() != '.svg':
            return p
        png = self.cfg.work / 'stills' / (p.stem + '.png')
        png.parent.mkdir(parents=True, exist_ok=True)
        if not png.exists() or png.stat().st_mtime < p.stat().st_mtime:
            subprocess.run(['rsvg-convert', '-w', str(self.size[0] * 2), '-o', str(png), str(p)], check=True)
        return png

    def card(self, shot: Shot) -> Path:
        meta, voice = self.narration(shot)
        length = max(float(shot.raw.get('seconds', 0)), 0.6 + meta['duration'] + shot.hold)
        video = shot.raw.get('video')  # a hydrapy render behind the text
        seg = self.out / f'{shot.key}.mkv'
        w = self._writer(seg)
        src = Frames(self.cfg.work / 'renders' / f'{video}.mp4', 0, size=self.size) if video else None
        still = None if src else self._background(shot)
        title, sub = fill(self.cfg, shot.raw.get('text', shot.title)), fill(self.cfg, shot.raw.get('sub', ''))
        H = self.size[1]
        s = H / 1080
        f1, f2 = font(self.cfg, int(84 * s)), font(self.cfg, int(36 * s))
        n = int(round(length * self.fps))
        for i in range(n):
            base = src.read() if src else still
            if base is None and src:  # the background loops
                src.close()
                src = Frames(self.cfg.work / 'renders' / f'{video}.mp4', 0, size=self.size)
                base = src.read()
            if base is None:
                base = np.zeros((H, self.size[0], 3), np.uint8)
            t = i / self.fps
            a = min(1.0, t / 0.5, (length - t) / 0.4)
            img = Image.fromarray((base * 0.55).astype(np.uint8)).convert('RGBA')
            layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
            d = ImageDraw.Draw(layer)
            x, y = 120 * s, H * 0.42
            d.text((x, y), title, font=f1, fill=(240, 236, 228, int(255 * a)))
            if sub:
                d.text((x, y + 110 * s), sub, font=f2, fill=(255, 199, 51, int(255 * a)))
            w.write(np.asarray(Image.alpha_composite(img, layer).convert('RGB')))
        w.close()
        if src:
            src.close()
        self._mix(shot, voice, 0.6, None, length, seg.with_suffix('.wav'))
        self._captions(shot, meta, 0.6, seg)
        return seg

    def still(self, shot: Shot) -> Path:
        meta, voice = self.narration(shot)
        p = Path(shot.raw['asset'])
        p = p if p.is_absolute() else self.cfg.root / p
        img = cv2.cvtColor(cv2.imread(str(self._rasterise(p))), cv2.COLOR_BGR2RGB)
        length = 0.5 + meta['duration'] + shot.hold
        h, w = img.shape[:2]
        aspect = self.size[0] / self.size[1]
        cw, ch = (w, w / aspect) if w / h < aspect else (h * aspect, h)

        def cue_time(name):
            return 0.0 if name in (None, 0) else 0.5 + meta['cues'][name]
        # frames on a still: rect as fractions of the image; default a slow push-in
        keys = shot.frames or [dict(rect=[0, 0, 1, 1]), dict(at=None, rect=[0.03, 0.03, 0.94, 0.94], ease=length)]
        track = []
        for k in keys:
            rx, ry, rw, rh = k.get('rect', [0, 0, 1, 1])
            rw_px, rh_px = rw * w, rh * h
            if rw_px / rh_px > aspect:
                rh_px = rw_px / aspect
            else:
                rw_px = rh_px * aspect
            cx, cy = (rx + rw / 2) * w, (ry + rh / 2) * h
            track.append((cue_time(k.get('at')), [cx - rw_px / 2, cy - rh_px / 2, rw_px, rh_px], float(k.get('ease', 0.8))))
        over = overlay_windows(shot, cue_time, length)
        seg = self.out / f'{shot.key}.mkv'
        wr = self._writer(seg)
        for i in range(int(round(length * self.fps))):
            t = i / self.fps
            rgb = crop(img, rect_at(track, t), self.size)
            wr.write(draw_overlays(self.cfg, rgb, active_overlays(over, t)))
        wr.close()
        self._mix(shot, voice, 0.5, None, length, seg.with_suffix('.wav'))
        self._captions(shot, meta, 0.5, seg)
        return seg

    def render(self, shot: Shot) -> Path:
        """A clean offline HydraPy render (the picture at full quality), with narration over it."""
        r = shot.raw['render']
        out = self.cfg.work / 'renders' / f"{r.get('name', shot.key)}.mp4"
        if not out.exists():
            raise ComposeError(f'{shot.key}: {out} missing; run gtut renders')
        meta, voice = self.narration(shot)
        length = max(0.5 + meta['duration'] + shot.hold, float(r.get('seconds', 0)) if r.get('fit') == 'render' else 0)
        src = Frames(out, float(r.get('offset', 0)), size=self.size)
        seg = self.out / f'{shot.key}.mkv'
        wr = self._writer(seg)

        def cue_time(name):
            return 0.0 if name in (None, 0) else 0.5 + meta['cues'][name]
        over = overlay_windows(shot, cue_time, length)
        last = None
        for i in range(int(round(length * self.fps))):
            f = src.at(int(round(i / self.fps * src.fps)))
            last = f if f is not None else last
            wr.write(draw_overlays(self.cfg, last, active_overlays(over, i / self.fps)))
        src.close()
        wr.close()
        prog = read_audio(Path(r['audio']) if r.get('audio') else out, channels=2)
        self._mix(shot, voice, 0.5, prog, length, seg.with_suffix('.wav'))
        self._captions(shot, meta, 0.5, seg)
        return seg


# chapters and the whole ----------------------------------------------------------

def srt_time(t: float) -> str:
    ms = int(round(t * 1000))
    return f'{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}'


def join(cfg, segments: list[Path], out: Path, title: str, chapter_marks: list[tuple[float, str]] | None = None) -> Path:
    """Concatenate segments (video + wav), normalise loudness, write SRT and chapters."""
    out.parent.mkdir(parents=True, exist_ok=True)
    lst = out.with_suffix('.concat.txt')
    lst.write_text(''.join(f"file '{s.resolve()}'\n" for s in segments))
    audio_parts = [read_audio(s.with_suffix('.wav'), channels=2) for s in segments]
    durations = [probe_duration(s) for s in segments]
    # pad or trim each wav to its video length so the two stay together across cuts
    fixed = []
    for a, d in zip(audio_parts, durations):
        n = int(round(d * RATE))
        fixed.append(np.pad(a, ((0, max(0, n - len(a))), (0, 0)))[:n])
    mix = out.with_suffix('.mix.wav')
    write_wav(mix, np.concatenate(fixed) if fixed else np.zeros((1, 2)))
    caps, t = [], 0.0
    for s, d in zip(segments, durations):
        cp = s.with_suffix('.captions.json')
        if cp.exists():
            caps += [dict(start=c['start'] + t, end=c['end'] + t, text=c['text']) for c in json.loads(cp.read_text())]
        t += d
    srt = out.with_suffix('.srt')
    srt.write_text(''.join(f"{i}\n{srt_time(c['start'])} --> {srt_time(c['end'])}\n{c['text']}\n\n"
                           for i, c in enumerate(caps, 1)))
    meta = out.with_suffix('.ffmeta')
    lines = [';FFMETADATA1', f'title={title}']
    marks = chapter_marks or []
    total = sum(durations)
    for i, (start, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else total
        lines += ['[CHAPTER]', 'TIMEBASE=1/1000', f'START={int(start * 1000)}', f'END={int(end * 1000)}', f'title={name}']
    meta.write_text('\n'.join(lines) + '\n')
    o = cfg['output']
    target = o['loudness']
    stats = subprocess.run(['ffmpeg', '-hide_banner', '-i', str(mix), '-af',
                            f'loudnorm=I={target}:TP=-1.5:LRA=11:print_format=json', '-f', 'null', '-'],
                           capture_output=True, text=True).stderr
    m = json.loads(stats[stats.rindex('{'):stats.rindex('}') + 1])
    silent = m['input_i'] in ('-inf', 'inf') or float(m['input_i']) < -60
    norm = 'anull' if silent else (f"loudnorm=I={target}:TP=-1.5:LRA=11:measured_I={m['input_i']}:measured_TP={m['input_tp']}:"
            f"measured_LRA={m['input_lra']}:measured_thresh={m['input_thresh']}:offset={m['target_offset']}:linear=true")
    vf = []
    if o['captions'] in ('burn', 'both') and caps:
        vf = ['-vf', f"subtitles={srt}:force_style='FontName={o['font']},FontSize=22,Outline=1,Shadow=0,MarginV=40'"]
    args = ['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(lst), '-i', str(mix),
            '-i', str(meta), '-map', '0:v', '-map', '1:a', '-map_metadata', '2', '-map_chapters', '2',
            *vf, *video_args(o['encoder'], o['quality'], intermediate=False),
            '-af', norm, '-ar', '48000', '-c:a', 'aac', '-b:a', '256k', '-movflags', '+faststart', str(out)]
    subprocess.run(args, check=True)
    if o['captions'] == 'burn':
        srt.unlink()
    return out


def probe_duration(path: Path) -> float:
    info = probe_media(path)
    v = next(s for s in info['streams'] if s['codec_type'] == 'video')
    if v.get('nb_frames') and v.get('avg_frame_rate', '0/0') != '0/0':
        num, den = map(int, v['avg_frame_rate'].split('/'))
        return int(v['nb_frames']) / (num / den)
    return float(v.get('duration') or info['format']['duration'])


def chapter(cfg, ch: Chapter) -> Path:
    comp = Composer(cfg)
    segs = []
    for s in ch.shots:
        seg = comp.out / f'{s.key}.mkv'
        if not seg.exists():
            raise ComposeError(f'{s.key}: segment not composed; run gtut compose {ch.number}')
        segs.append(seg)
    return join(cfg, segs, cfg.work / 'chapters' / f'{ch.name}.mp4', f'{ch.number}. {ch.title}')


def tutorial(cfg, chapters: list[Chapter], name: str = 'gorgon-ecosystem-tutorial') -> Path:
    comp = Composer(cfg)
    segs, marks, t = [], [], 0.0
    for ch in chapters:
        marks.append((t, f'{ch.number}. {ch.title}'))
        for s in ch.shots:
            seg = comp.out / f'{s.key}.mkv'
            segs.append(seg)
            t += probe_duration(seg)
    out = join(cfg, segs, cfg.work / 'final' / f'{name}.mp4', 'Gorgon, HydraPy and the Rack modules', marks)
    out.with_suffix('.chapters.txt').write_text(
        ''.join(f'{int(m // 60):d}:{int(m % 60):02d} {n}\n' for m, n in marks))
    return out
