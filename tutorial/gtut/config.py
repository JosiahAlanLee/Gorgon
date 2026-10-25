"""Load config.yaml over the defaults in config.example.yaml."""
from __future__ import annotations

import copy
import os
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent.parent  # tutorial/


def _merge(base, over):
    if isinstance(base, dict) and isinstance(over, dict):
        out = dict(base)
        for k, v in over.items():
            out[k] = _merge(base.get(k), v) if k in base else v
        return out
    return copy.deepcopy(over)


class Config(dict):
    """A nested dict with attribute-free access and resolved paths."""

    def __init__(self, data, root: Path):
        super().__init__(data)
        self.root = root

    def path(self, key: str) -> Path | None:
        value = self['paths'].get(key)
        if value in (None, ''):
            return None
        p = Path(os.path.expanduser(str(value)))
        return p if p.is_absolute() else (self.root / p).resolve()

    @property
    def work(self) -> Path:
        w = self.path('work')
        w.mkdir(parents=True, exist_ok=True)
        return w

    def stage_dir(self) -> Path:
        d = self.work / 'stage'
        d.mkdir(parents=True, exist_ok=True)
        return d

    @property
    def display(self) -> str:
        return f":{self['stage']['display_number']}"


def load(path: str | os.PathLike | None = None) -> Config:
    defaults = yaml.safe_load((HERE / 'config.example.yaml').read_text())
    path = Path(path) if path else Path(os.environ.get('GTUT_CONFIG', HERE / 'config.yaml'))
    data = defaults
    root = HERE
    if path.exists():
        data = _merge(defaults, yaml.safe_load(path.read_text()) or {})
        root = path.resolve().parent
    return Config(data, root)
