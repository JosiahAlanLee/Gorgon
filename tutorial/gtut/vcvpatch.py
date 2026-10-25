"""Write VCV Rack 2 patches by module, port and param *names*.

Names come from the catalog the Director probe writes (`gtut catalog`): every
installed model's params (with ranges) and ports, by index. Rack 2 loads a plain
JSON .vcv, as OTIS-start.vcv in DSP_Emulation shows.

    p = Patch(catalog)
    g = p.add('GorgonBridge', 'Gorgon', x=0)
    v = p.add('VastishLab', 'Vastish', x=14)
    p.cable(g, 'V/OCT', v, 'V/OCT')
    p.set(v, 'Master', 0.6)
    p.save('work/patches/c07-first-voice.vcv')

Module positions are in HP (x) and rack rows (y). A Director module is added at
the far right of row 0 unless `director=False`.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path


class PatchError(ValueError):
    pass


def load_catalog(path: Path) -> dict:
    return json.loads(Path(path).read_text())


def _pick(items: list[dict], want, what: str) -> dict:
    if isinstance(want, str) and want.startswith('#') and want[1:].isdigit():
        want = int(want[1:])
    if isinstance(want, int):
        for it in items:
            if it['index'] == want:
                return it
        raise PatchError(f'{what}: no index {want}')
    w = str(want).lower()
    for rule in (lambda n: n == w, lambda n: n.startswith(w), lambda n: w in n):
        hits = [it for it in items if rule(it['name'].lower())]
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            raise PatchError(f'{what}: {want!r} is ambiguous among {[h["name"] for h in hits]}')
    raise PatchError(f'{what}: no {want!r}; have {[it["name"] for it in items]}')


class Module:
    def __init__(self, patch, mid: int, plugin: str, model: str, spec: dict):
        self.patch, self.id, self.plugin, self.model, self.spec = patch, mid, plugin, model, spec

    def __repr__(self):
        return f'<{self.model} #{self.id}>'


class Patch:
    def __init__(self, catalog: dict, rack_version: str = '2.6.6'):
        self.catalog = catalog
        self.version = rack_version
        self.modules: list[dict] = []
        self.cables: list[dict] = []
        self._next = 1
        self._handles: dict[int, Module] = {}

    def spec(self, plugin: str, model: str) -> dict:
        try:
            return self.catalog[plugin]['models'][model]
        except KeyError:
            have = {p: list(v['models']) for p, v in self.catalog.items() if p == plugin} or list(self.catalog)
            raise PatchError(f'{plugin}/{model} is not in the catalog; have {have}') from None

    def add(self, plugin: str, model: str, x: int | None = None, y: int = 0, params: dict | None = None,
            data: dict | None = None, bypass: bool = False, gap: int = 0) -> Module:
        """x None: just right of the rightmost module in row y (gap HP between), using the
        catalog's panel widths, so a row of adjacent modules (expanders, sample links) touches."""
        spec = self.spec(plugin, model)
        if x is None:
            x = max((e['pos'][0] + self._hp(e) for e in self.modules if e['pos'][1] == y), default=0) + gap
        mid = self._next
        self._next += 1
        entry = dict(id=mid, plugin=plugin, model=model, version=self.catalog[plugin]['version'],
                     pos=[int(x), int(y)],
                     params=[dict(id=p['index'], value=p['default']) for p in spec['params']])
        if data is not None:
            entry['data'] = copy.deepcopy(data)
        if bypass:
            entry['bypass'] = True
        self.modules.append(entry)
        handle = Module(self, mid, plugin, model, spec)
        self._handles[mid] = handle
        for k, v in (params or {}).items():
            self.set(handle, k, v)
        return handle

    def _hp(self, entry: dict) -> int:
        hp = self.catalog[entry['plugin']]['models'][entry['model']].get('hp')
        if hp is None:
            raise PatchError(f"{entry['model']}: no panel width in the catalog; give x explicitly or rerun gtut catalog")
        return int(hp)

    def _entry(self, m: Module) -> dict:
        return next(e for e in self.modules if e['id'] == m.id)

    def set(self, m: Module, param, value: float, scaled: bool = False):
        p = _pick(m.spec['params'], param, f'{m.model} param')
        v = p['min'] + value * (p['max'] - p['min']) if scaled else value
        if not (min(p['min'], p['max']) - 1e-6 <= v <= max(p['min'], p['max']) + 1e-6):
            raise PatchError(f'{m.model}.{p["name"]} = {v} is outside {p["min"]}..{p["max"]}')
        for e in self._entry(m)['params']:
            if e['id'] == p['index']:
                e['value'] = v
        return self

    def data(self, m: Module, data: dict):
        self._entry(m)['data'] = copy.deepcopy(data)
        return self

    def cable(self, src: Module, out, dst: Module, inp, color: str | None = None):
        o = _pick(src.spec['outputs'], out, f'{src.model} output')
        i = _pick(dst.spec['inputs'], inp, f'{dst.model} input')
        for c in self.cables:
            if c['inputModuleId'] == dst.id and c['inputId'] == i['index']:
                raise PatchError(f'{dst.model}.{i["name"]} already has a cable')
        cable = dict(id=len(self.cables) + 1, outputModuleId=src.id, outputId=o['index'],
                     inputModuleId=dst.id, inputId=i['index'])
        if color:
            cable['color'] = color
        self.cables.append(cable)
        return self

    def explain(self) -> list[str]:
        """Every cable with the names it resolved to, for a human to check the patch means what it should."""
        lines = []
        for c in self.cables:
            src, dst = self._handles[c['outputModuleId']], self._handles[c['inputModuleId']]
            o = next(p for p in src.spec['outputs'] if p['index'] == c['outputId'])
            i = next(p for p in dst.spec['inputs'] if p['index'] == c['inputId'])
            lines.append(f"{src.model}.{o['name']}  ->  {dst.model}.{i['name']}")
        return lines

    def to_json(self, director: bool = True) -> dict:
        modules = list(self.modules)
        if director and not any(m['model'] == 'Director' for m in modules):
            right = max((m['pos'][0] + self._hp(m) for m in modules if m['pos'][1] == 0), default=0)
            modules.append(dict(id=self._next, plugin='GorgonTutorialProbe', model='Director',
                                version=self.catalog.get('GorgonTutorialProbe', {}).get('version', '2.0.0'),
                                pos=[right + 40, 0], params=[]))  # well clear of the shots
        return dict(version=self.version, modules=modules, cables=self.cables)

    def save(self, path, director: bool = True) -> Path:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_json(director), indent=1))
        return path


def harvest(saved_patch: Path, plugin: str, model: str) -> dict | None:
    """The `data` block of the first such module in a patch saved from Rack.

    For state only the GUI sets reliably (an Audio module's JACK device, Gorgon's
    capture device): configure it once by hand or with RackDriver.menu, save with
    the probe, and reuse the block in generated patches.
    """
    text = Path(saved_patch).read_bytes()
    if text[:4] == b'\x28\xb5\x2f\xfd':  # zstd: a Rack-saved .vcv archive
        import io
        import tarfile
        import zstandard
        raw = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(text)).read()
        with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
            text = tar.extractfile('./patch.json' if './patch.json' in tar.getnames() else 'patch.json').read()
    patch = json.loads(text)
    for m in patch['modules']:
        if m['plugin'] == plugin and m['model'] == model:
            return m.get('data')
    return None
