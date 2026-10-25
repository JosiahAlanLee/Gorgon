"""Chapter scripts: YAML files of shots, each with narration, cues, actions and framing.

A narration may contain cue marks, `{name}`, between words. Actions can wait for a
cue (`at: name`), so the hand moves when the voice says so. Cue times come from
the narration audio (narrate.py), not from guesses.

Shot keys:
  id, title           required; id is unique within the chapter
  source              docs that back the narration (repo-relative path#section)
  verify              claims to check on the real build before recording
  needs               stage services: studio, controller, program, rack, terminal
  setup               state before the take: studio {patch, score, transport}, rack {patch}
  narration           text with {cue} marks; empty for a silent shot
  actions             list; each has one action key plus optional at/after/mark
  frames              framing keyframes [{at, window, rect, zoom}]; default: the first window in needs
  overlays            [{at, until, kind, text}]
  hold                seconds to keep rolling after the narration and actions end
  speed               [{from: mark, to: mark, factor: 8}] fast-forward ranges (long installs)
  kind                live (default: a take on the stage) | card (title card: text, sub) |
                      still (asset: an image, e.g. a diagram SVG) | render (render: hydrapy render args)
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

import yaml

CUE = re.compile(r'\{([a-z0-9_-]+)\}')
CONTROL = {'at', 'after', 'mark', 'note'}
SERVICES = {'studio', 'controller', 'program', 'aux', 'rack', 'terminal'}


class ScriptError(ValueError):
    pass


@dataclass
class Shot:
    chapter: str
    id: str
    title: str
    narration: str = ''
    actions: list = field(default_factory=list)
    needs: list = field(default_factory=list)
    setup: dict = field(default_factory=dict)
    frames: list = field(default_factory=list)
    overlays: list = field(default_factory=list)
    source: list = field(default_factory=list)
    verify: list = field(default_factory=list)
    hold: float = 1.0
    speed: list = field(default_factory=list)
    kind: str = 'live'  # live | card | still | render
    raw: dict = field(default_factory=dict)  # every key as written (card text, asset, render args)

    @property
    def key(self) -> str:
        return f'{self.chapter}-{self.id}'

    def cues(self) -> list[str]:
        return CUE.findall(self.narration or '')

    def spoken(self) -> str:
        """The narration as it is read: cue marks removed, whitespace normalised."""
        return re.sub(r'\s+', ' ', CUE.sub('', self.narration or '')).strip()


@dataclass
class Chapter:
    path: Path
    number: str
    slug: str
    title: str
    shots: list
    defaults: dict

    @property
    def name(self) -> str:
        return f'{self.number}-{self.slug}'


def load_chapter(path: Path) -> Chapter:
    data = yaml.safe_load(Path(path).read_text())
    number = str(data['chapter']).zfill(2)
    defaults = data.get('defaults', {})
    shots = []
    seen = set()
    for raw in data.get('shots', []):
        raw = {**defaults, **raw}
        if raw['id'] in seen:
            raise ScriptError(f'{path}: duplicate shot id {raw["id"]}')
        seen.add(raw['id'])
        shot = Shot(chapter=number, id=raw['id'], title=raw.get('title', raw['id']),
                    narration=raw.get('narration', '') or '', actions=raw.get('actions', []) or [],
                    needs=raw.get('needs', []) or [], setup=raw.get('setup', {}) or {},
                    frames=raw.get('frames', []) or [], overlays=raw.get('overlays', []) or [],
                    source=_list(raw.get('source')), verify=_list(raw.get('verify')),
                    hold=float(raw.get('hold', 1.0)), speed=raw.get('speed', []) or [],
                    kind=raw.get('kind', 'live'), raw=raw)
        validate(shot, path)
        shots.append(shot)
    return Chapter(Path(path), number, data['slug'], data['title'], shots, defaults)


def _list(v):
    if v is None:
        return []
    return v if isinstance(v, list) else [v]


def validate(shot: Shot, path):
    where = f'{path}:{shot.id}'
    bad = set(shot.needs) - SERVICES
    if bad:
        raise ScriptError(f'{where}: unknown needs {bad}')
    cues = set(shot.cues())
    marks = set()
    from .actions import ACTIONS  # late: actions imports drivers
    for i, a in enumerate(shot.actions):
        if not isinstance(a, dict):
            raise ScriptError(f'{where}: action {i} is not a mapping')
        keys = [k for k in a if k not in CONTROL]
        if len(keys) != 1:
            raise ScriptError(f'{where}: action {i} needs exactly one action key, has {keys}')
        if keys[0] not in ACTIONS:
            raise ScriptError(f'{where}: unknown action {keys[0]!r}; known: {sorted(ACTIONS)}')
        if 'at' in a and a['at'] not in cues:
            raise ScriptError(f'{where}: action {i} waits for cue {a["at"]!r}, not in the narration')
        if 'mark' in a:
            marks.add(a['mark'])
    service = {'studio': 'controller', 'program': 'program', 'rack': 'rack', 'terminal': 'terminal'}
    for f in shot.frames:
        w = f.get('window')
        if shot.kind == 'live' and w and service.get(w) not in shot.needs:
            raise ScriptError(f'{where}: frames show {w!r} but needs has no {service.get(w)!r}')
        at = f.get('at')
        if at not in (None, 0) and at not in cues and at not in marks:
            raise ScriptError(f'{where}: frame at {at!r} is neither a cue nor a mark')
    for o in shot.overlays:
        for k in ('at', 'until'):
            v = o.get(k)
            if isinstance(v, str) and not v.startswith('+') and v not in cues and v not in marks:
                raise ScriptError(f'{where}: overlay {k} {v!r} is neither a cue nor a mark')


def chapters(script_dir: Path) -> list[Chapter]:
    return [load_chapter(p) for p in sorted(Path(script_dir).glob('[0-9][0-9]-*.yaml'))]


def find(script_dir: Path, ref: str) -> tuple[Chapter, list[Shot]]:
    """'05' or '05-tracker' or '05:play-vs-edit' or '05:play-vs-edit,seek'."""
    num, _, ids = ref.partition(':')
    num = num.split('-')[0].zfill(2)
    for ch in chapters(script_dir):
        if ch.number == num:
            if not ids:
                return ch, ch.shots
            want = ids.split(',')
            picked = [s for s in ch.shots if s.id in want]
            missing = set(want) - {s.id for s in picked}
            if missing:
                raise ScriptError(f'chapter {num} has no shots {missing}')
            return ch, picked
    raise ScriptError(f'no chapter {num} in {script_dir}')
