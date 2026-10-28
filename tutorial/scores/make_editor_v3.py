"""Generate editor-v3.json from the legacy demo without changing its musical content."""
import copy
import json
from pathlib import Path

here = Path(__file__).resolve().parent
doc = copy.deepcopy(json.loads((here / 'demo.json').read_text()))
doc.update(format='hydrapy-tracker-3', chain=list(range(4)), seed=0,
           lanes=[{'name': n} for n in ['Breaks', 'FM hits', 'Sub', 'Texture', 'Lane 5', 'Lane 6']],
           mutes=doc['mutes'] + [False, False],
           sounds=[{'machine': None, 'params': {}} for _ in range(6)])
for pattern in doc['patterns']:
    audio = pattern.pop('audio') + [[None] * 64 for _ in range(2)]
    pattern.update(rate=1, bar=16, bars=4,
                   lanes=[{'len': 64, 'cells': cells, 'transforms': []} for cells in audio])
doc['patterns'] += [None] * 124
(here / 'editor-v3.json').write_text(json.dumps(doc, indent=1) + '\n')
