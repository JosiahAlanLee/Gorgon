"""Checks that make a take, a segment or a chapter reviewable without watching it.

Contact sheets are for looking at (Claude Code reads the PNGs); the rest are
measurements with thresholds. Nothing here passes a video on its own: the report
lists what was checked and what was not.
"""
from __future__ import annotations

import getpass
import json
import re
import shutil
import socket
import subprocess
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw

from .compose import Frames, probe_duration, font


def contact_sheet(cfg, video: Path, out: Path, cols: int = 4, rows: int = 4, width: int = 1920,
                  start: float = 0.0, end: float | None = None) -> Path:
    dur = probe_duration(video)
    end = dur if end is None else min(end, dur)
    n = cols * rows
    times = [start + (end - start) * (i + 0.5) / n for i in range(n)]
    tw = width // cols
    tiles = []
    for t in times:
        fr = Frames(video, t)
        f = fr.read()
        fr.close()
        if f is None:
            f = np.zeros((90, 160, 3), np.uint8)
        th = int(tw * f.shape[0] / f.shape[1])
        tile = Image.fromarray(cv2.resize(f, (tw, th), interpolation=cv2.INTER_AREA))
        d = ImageDraw.Draw(tile)
        label = f'{int(t // 60)}:{t % 60:05.2f}'
        d.rectangle([0, 0, 120, 30], fill=(0, 0, 0))
        d.text((6, 4), label, font=font(cfg, 20), fill=(255, 220, 120))
        tiles.append(tile)
    th = tiles[0].height
    sheet = Image.new('RGB', (tw * cols, th * rows), (8, 8, 8))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % cols) * tw, (i // cols) * th))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return out


def _ffmpeg_filter_log(video: Path, af: str | None = None, vf: str | None = None) -> str:
    args = ['ffmpeg', '-hide_banner', '-nostats', '-i', str(video)]
    if vf:
        args += ['-vf', vf]
    else:
        args += ['-vn']
    if af:
        args += ['-af', af]
    else:
        args += ['-an']
    args += ['-f', 'null', '-']
    return subprocess.run(args, capture_output=True, text=True).stderr


def loudness(video: Path) -> dict:
    log = _ffmpeg_filter_log(video, af='ebur128=peak=true')
    i = re.findall(r'I:\s+(-?[\d.]+) LUFS', log)
    tp = re.findall(r'Peak:\s+(-?[\d.]+) dBFS', log)
    return dict(integrated=float(i[-1]) if i else None, true_peak=float(tp[-1]) if tp else None)


def freezes(video: Path, seconds: float = 8.0) -> list[tuple[float, float]]:
    log = _ffmpeg_filter_log(video, vf=f'freezedetect=n=0.002:d={seconds}')
    starts = [float(x) for x in re.findall(r'freeze_start: ([\d.]+)', log)]
    ends = [float(x) for x in re.findall(r'freeze_end: ([\d.]+)', log)]
    return list(zip(starts, ends + [None] * (len(starts) - len(ends))))


def blacks(video: Path) -> list[tuple[float, float]]:
    log = _ffmpeg_filter_log(video, vf='blackdetect=d=0.5:pic_th=0.98:pix_th=0.03')
    return [(float(a), float(b)) for a, b in re.findall(r'black_start:([\d.]+) black_end:([\d.]+)', log)]


def secrets(cfg) -> list[str]:
    """Strings that must never appear on screen or in captions."""
    out = []
    bridge = cfg.stage_dir() / 'studio.json'
    if bridge.exists():
        try:
            out.append(json.loads(bridge.read_text())['token'])
        except (ValueError, KeyError):
            pass
    for v in (getpass.getuser(), socket.gethostname(), str(Path.home())):
        if v and len(v) > 2 and v not in ('root', 'claude'):
            out.append(v)
    out += [s for s in cfg.get('qc', {}).get('never_show', []) if s]
    return out


def ocr_scan(cfg, video: Path, every: float = 2.0) -> list[dict]:
    """OCR sampled frames for anything in secrets(); needs tesseract."""
    if not shutil.which('tesseract'):
        return [dict(warning='tesseract not installed: on-screen text was not scanned')]
    words = secrets(cfg)
    hits = []
    dur = probe_duration(video)
    t = 0.5
    tmp = cfg.work / 'qc' / '_ocr.png'
    tmp.parent.mkdir(parents=True, exist_ok=True)
    while t < dur:
        fr = Frames(video, t)
        f = fr.read()
        fr.close()
        if f is not None:
            Image.fromarray(f).save(tmp)
            text = subprocess.run(['tesseract', str(tmp), '-', '--psm', '11'], capture_output=True, text=True).stdout
            for w in words:
                probe = w if len(w) < 12 else w[:10]  # OCR rarely reads a long token whole
                if probe and probe in text:
                    hits.append(dict(time=round(t, 2), found=('session token' if len(w) > 30 else w)))
        t += every
    return hits


def text_scan(cfg, paths: list[Path]) -> list[dict]:
    words = secrets(cfg)
    hits = []
    for p in paths:
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        for w in words:
            if w in text:
                hits.append(dict(file=str(p), found=('session token' if len(w) > 30 else w)))
    return hits


def take_report(cfg, take_dir: Path) -> dict:
    take = json.loads((take_dir / 'take.json').read_text())
    from .compose import find_slate
    slate = find_slate(take_dir / 'take.mkv', take.get('slate_wall'))
    problems = []
    log = (take_dir / 'take.ffmpeg.log').read_text(errors='replace') if (take_dir / 'take.ffmpeg.log').exists() else ''
    xruns = log.count('xrun')
    if xruns > 3:
        problems.append(f'{xruns} JACK xruns while recording: programme sound has gaps and may drift (machine overloaded?)')
    if take.get('error'):
        problems.append('take raised an error (see take.json)')
    if not slate['found']:
        problems.append(f'slate not found (video {slate["video"]}, audio {slate["audio"]})')
    elif abs(slate['offset']) > 0.2:
        problems.append(f'A/V offset {slate["offset"]*1000:.0f} ms is large; compose corrects it, but check the pipeline')
    late = [e for e in take['log'] if e.get('late', 0) > 0.4]
    for e in late:
        problems.append(f"{e['action']} started {e['late']:.2f}s after its cue: the narration may be ahead of the picture")
    if take.get('rack_fps') is not None and take['rack_fps'] < 0.8 * cfg['capture']['fps']:
        problems.append(f"Rack drew at {take['rack_fps']:.0f} fps: below capture rate (try stage.display xvfb+virtualgl)")
    sheet = contact_sheet(cfg, take_dir / 'take.mkv', take_dir / 'contact.png')
    report = dict(take=str(take_dir), slate=slate, problems=problems, contact_sheet=str(sheet),
                  actions=len([e for e in take['log'] if 'action' in e]))
    (take_dir / 'qc.json').write_text(json.dumps(report, indent=1))
    return report


def video_report(cfg, video: Path, kind: str = 'chapter', ocr: bool = True) -> dict:
    out_dir = cfg.work / 'qc'
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = video.stem
    problems, notes = [], []
    loud = loudness(video)
    target = cfg['output']['loudness']
    if loud['integrated'] is None or abs(loud['integrated'] - target) > 1.0:
        problems.append(f"integrated loudness {loud['integrated']} LUFS, target {target}")
    if loud['true_peak'] is not None and loud['true_peak'] > -1.0:
        problems.append(f"true peak {loud['true_peak']} dBFS (> -1)")
    for a, b in blacks(video):
        problems.append(f'black from {a:.1f}s to {b:.1f}s')
    for a, b in freezes(video):
        notes.append(f'picture still from {a:.1f}s to {b if b is None else round(b, 1)}s (fine for a card or still; not for a live shot)')
    hits = ocr_scan(cfg, video) if ocr else [dict(warning='OCR skipped')]
    for h in hits:
        if 'found' in h:
            problems.append(f"PRIVATE TEXT ON SCREEN at {h['time']}s: {h['found']}")
        else:
            notes.append(h['warning'])
    srt = video.with_suffix('.srt')
    for h in text_scan(cfg, [srt, cfg.stage_dir() / 'terminal.log']):
        problems.append(f"private text in {h['file']}: {h['found']}")
    sheets = []
    dur = probe_duration(video)
    per = 120.0
    k = 0
    t = 0.0
    while t < dur:
        sheets.append(str(contact_sheet(cfg, video, out_dir / f'{stem}-{k:02}.png', start=t, end=min(dur, t + per))))
        t += per
        k += 1
    report = dict(video=str(video), duration=dur, loudness=loud, problems=problems, notes=notes, contact_sheets=sheets,
                  not_checked=['whether narration is true of the current build (see each shot\'s verify list)',
                               'pronunciation and pacing (listen)', 'legibility at phone size (look at the sheets)'])
    (out_dir / f'{stem}.json').write_text(json.dumps(report, indent=1))
    return report
