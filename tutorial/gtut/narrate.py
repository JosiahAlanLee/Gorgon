"""Narration: speech for each shot, its cue times and its captions.

Speech is made a sentence at a time, so sentence boundaries (captions) are exact.
A cue inside a sentence is placed by word timings from faster-whisper when it is
installed (align: auto|whisper), otherwise in proportion to its character position
in the sentence.

Engines:
  kokoro    Kokoro-82M (Apache-2.0) on the GPU; `pip install kokoro soundfile`, espeak-ng installed
  piper     Piper; narration.voice is the .onnx model path
  recorded  your own reading: work/narration/recorded/<chapter>-<shot>.wav per shot
            (`gtut narrate --prompter` writes the script to read)
  espeak    espeak-ng: robotic but instant, for rehearsing timing before the real voice
  none      silence of a reading-pace length; for rehearsing takes before a voice is chosen
"""
from __future__ import annotations

import difflib
import hashlib
import json
import re
import shutil
import subprocess
import wave
from pathlib import Path

import numpy as np

from .script import CUE, Shot

RATE = 48000
SENTENCE = re.compile(r'(?<=[.!?])\s+(?=[A-Z0-9"“(])')


def sentences(narration: str) -> list[str]:
    text = re.sub(r'\s+', ' ', narration or '').strip()
    return [s for s in SENTENCE.split(text) if s.strip()] if text else []


def spoken(s: str) -> str:
    return re.sub(r'\s+', ' ', CUE.sub('', s)).strip()


def speakable(s: str, lexicon: dict | None = None) -> str:
    """What the engine reads: cue marks removed and notation spelled out (narration.lexicon).

    Lexicon keys are regexes matched on word boundaries unless they contain their own anchors.
    """
    s = spoken(s)
    s = re.sub(r'\s*—\s*', ', ', re.sub(r'`([^`]*)`', r'\1', s))
    for pat, rep in (lexicon or {}).items():
        rx = pat if any(c in pat for c in '\\^$') else r'\b' + re.escape(pat) + r'\b'
        s = re.sub(rx, rep, s)
    return s


def _write_wav(path: Path, audio: np.ndarray, rate: int = RATE):
    audio = np.clip(audio, -1, 1)
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((audio * 32767).astype('<i2').tobytes())


def _read_wav(path: Path) -> np.ndarray:
    out = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-ac', '1', '-ar', str(RATE), '-f', 'f32le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype='<f4').copy()


def _resample(audio: np.ndarray, rate: int) -> np.ndarray:
    if rate == RATE:
        return audio.astype(np.float32)
    n = int(round(len(audio) * RATE / rate))
    return np.interp(np.linspace(0, len(audio) - 1, n), np.arange(len(audio)), audio).astype(np.float32)


class Engine:
    def __init__(self, cfg):
        self.cfg = cfg
        n = cfg['narration']
        self.name, self.voice, self.speed = n['engine'], n['voice'], float(n['speed'])
        self._kokoro = None

    def say(self, text: str) -> np.ndarray:
        if self.name == 'kokoro':
            if self._kokoro is None:
                from kokoro import KPipeline
                self._kokoro = KPipeline(lang_code=self.voice[0] if self.voice else 'a')
            parts = [np.asarray(r.audio if hasattr(r, 'audio') else r[2], dtype=np.float32)
                     for r in self._kokoro(text, voice=self.voice, speed=self.speed, split_pattern=None)]
            return _resample(np.concatenate(parts) if parts else np.zeros(1), 24000)
        if self.name == 'piper':
            if not shutil.which('piper'):
                raise RuntimeError('narration.engine is piper but piper is not on PATH')
            out = subprocess.run(['piper', '--model', str(self.voice), '--output-raw',
                                  '--length_scale', str(1 / self.speed)], input=text.encode(),
                                 capture_output=True, check=True).stdout
            cfg_json = Path(str(self.voice) + '.json')
            rate = json.loads(cfg_json.read_text())['audio']['sample_rate'] if cfg_json.exists() else 22050
            return _resample(np.frombuffer(out, dtype='<i2').astype(np.float32) / 32768, rate)
        if self.name == 'espeak':  # robotic, instant: for rehearsing timing only
            exe = shutil.which('espeak-ng') or shutil.which('espeak')
            out = subprocess.run([exe, '-v', 'en-us', '-s', str(int(165 * self.speed)), '--stdout', text],
                                 capture_output=True, check=True).stdout
            with wave.open(__import__('io').BytesIO(out)) as w:
                rate = w.getframerate()
                pcm = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float32) / 32768
            return _resample(pcm, rate)
        if self.name == 'none':
            words = len(text.split())
            return np.zeros(int(RATE * max(0.6, words / 2.6)), dtype=np.float32)
        raise RuntimeError(f'engine {self.name} does not synthesise')


def _trim(audio: np.ndarray, threshold: float = 0.004) -> np.ndarray:
    loud = np.flatnonzero(np.abs(audio) > threshold)
    if len(loud) == 0:
        return audio
    pad = int(0.03 * RATE)
    return audio[max(0, loud[0] - pad): loud[-1] + pad]


def _words(s: str) -> list[str]:
    return re.findall(r"[a-z0-9']+", s.lower())


def _whisper_words(path: Path):
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        return None
    model = WhisperModel('small.en', device='auto', compute_type='auto')
    segs, _ = model.transcribe(str(path), word_timestamps=True, language='en')
    return [(w.word, w.start) for s in segs for w in (s.words or [])]


def _cue_times_whisper(text: str, voice: Path, cue_word_index: dict[str, int]):
    recognised = _whisper_words(voice)
    if not recognised:
        return None
    script = _words(spoken(text))
    heard = [_words(w)[0] if _words(w) else '' for w, _ in recognised]
    sm = difflib.SequenceMatcher(a=script, b=heard, autojunk=False)
    mapping = {}
    for a, b, n in sm.get_matching_blocks():
        for k in range(n):
            mapping[a + k] = recognised[b + k][1]
    out = {}
    for cue, idx in cue_word_index.items():
        known = sorted(mapping)
        if not known:
            return None
        after = [k for k in known if k >= idx]
        before = [k for k in known if k < idx]
        if after and after[0] == idx:
            out[cue] = mapping[idx]
        elif after and before:
            k0, k1 = before[-1], after[0]
            out[cue] = mapping[k0] + (mapping[k1] - mapping[k0]) * (idx - k0) / (k1 - k0)
        else:
            out[cue] = mapping[after[0]] if after else mapping[before[-1]]
    return out


def narrate_shot(cfg, shot: Shot, out_root: Path, force: bool = False) -> dict:
    n = cfg['narration']
    out = out_root / shot.key
    out.mkdir(parents=True, exist_ok=True)
    meta_path = out / 'meta.json'
    digest = hashlib.sha1(json.dumps([shot.narration, n['engine'], n['voice'], n['speed'],
                                      n['pause_after_sentence']]).encode()).hexdigest()[:12]
    if not force and meta_path.exists():
        meta = json.loads(meta_path.read_text())
        if meta.get('digest') == digest and (out / 'voice.wav').exists():
            return meta
    voice = out / 'voice.wav'
    parts = sentences(shot.narration)
    captions, cues, cue_word_index = [], {}, {}
    pause = float(n['pause_after_sentence'])

    if n['engine'] == 'recorded':
        src = out_root / 'recorded' / f'{shot.key}.wav'
        if not src.exists():
            raise FileNotFoundError(f'{src}: record this shot (see gtut narrate --prompter)')
        audio = _trim(_read_wav(src))
        _write_wav(voice, audio)
        total = len(audio) / RATE
        chars = sum(len(spoken(p)) for p in parts) or 1
        t = 0.0
        for p in parts:  # sentence times by length; whisper refines cues below
            d = total * len(spoken(p)) / chars
            captions.append(dict(start=t, end=t + d, text=spoken(p)))
            t += d
    else:
        engine = Engine(cfg)
        chunks, t = [], 0.0
        for p in parts:
            audio = _trim(engine.say(speakable(p, n.get('lexicon')))) if n['engine'] != 'none' else engine.say(spoken(p))
            d = len(audio) / RATE
            captions.append(dict(start=t, end=t + d, text=spoken(p)))
            chunks += [audio, np.zeros(int(pause * RATE), dtype=np.float32)]
            t += d + pause
        audio = np.concatenate(chunks) if chunks else np.zeros(1, dtype=np.float32)
        _write_wav(voice, audio)
        total = len(audio) / RATE

    # Cues: proportional within their sentence first ...
    words_before = 0
    for p, cap in zip(parts, captions):
        plain_len = len(spoken(p)) or 1
        for m in CUE.finditer(p):
            before = spoken(p[:m.start()])
            frac = len(before) / plain_len
            cues[m.group(1)] = cap['start'] + frac * (cap['end'] - cap['start'])
            cue_word_index[m.group(1)] = words_before + len(_words(before))
        words_before += len(_words(spoken(p)))
    method = 'proportional'
    # ... then by heard word timings, where available.
    if cues and n['align'] in ('auto', 'whisper') and n['engine'] != 'none':
        refined = _cue_times_whisper(shot.narration, voice, cue_word_index)
        if refined:
            cues.update(refined)
            method = 'whisper'
        elif n['align'] == 'whisper':
            raise RuntimeError('narration.align is whisper but faster-whisper is not installed')
    meta = dict(digest=digest, duration=total, cues=cues, captions=captions, cue_method=method,
                engine=n['engine'], voice=n['voice'])
    meta_path.write_text(json.dumps(meta, indent=1))
    return meta


def prompter(chapters, path: Path):
    """A reading script for `recorded` narration: one file per shot, cues shown as ▸."""
    lines = ['# Narration to record', '',
             'One WAV per shot, named as the heading, into work/narration/recorded/.',
             'Read at an even pace; ▸ marks where something happens on screen.', '']
    for ch in chapters:
        for s in ch.shots:
            if s.narration.strip():
                lines += [f'## {s.key}.wav', '', CUE.sub('▸', s.narration).strip(), '']
    path.write_text('\n'.join(lines))
    return path
