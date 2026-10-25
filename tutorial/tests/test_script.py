from pathlib import Path

import pytest

from gtut import script
from gtut.narrate import sentences, speakable

HERE = Path(__file__).resolve().parent.parent


def test_every_chapter_validates():
    chapters = script.chapters(HERE / 'script')
    assert [c.number for c in chapters] == [f'{i:02}' for i in range(len(chapters))]
    keys = [s.key for c in chapters for s in c.shots]
    assert len(keys) == len(set(keys))


def _write(tmp_path, body):
    p = tmp_path / '01-x.yaml'
    p.write_text('chapter: 1\nslug: x\ntitle: X\nshots:\n' + body)
    return p


def test_unknown_cue_and_action(tmp_path):
    with pytest.raises(script.ScriptError, match='cue'):
        script.load_chapter(_write(tmp_path, "  - id: a\n    needs: [controller]\n    narration: Hi {go}.\n"
                                             "    actions:\n      - at: went\n        studio.press: Space\n"))
    with pytest.raises(script.ScriptError, match='unknown action'):
        script.load_chapter(_write(tmp_path, "  - id: a\n    needs: [controller]\n    actions:\n      - studio.dance: 1\n"))
    with pytest.raises(script.ScriptError, match='needs'):
        script.load_chapter(_write(tmp_path, "  - id: a\n    needs: [controller]\n    frames:\n      - {window: rack}\n"))


def test_cues_and_sentences():
    s = script.Shot(chapter='01', id='a', title='a', narration='Press Space. {play} The row moves.')
    assert s.cues() == ['play'] and s.spoken() == 'Press Space. The row moves.'
    assert sentences('One. Two {c} three! E4 rows.') == ['One.', 'Two {c} three!', 'E4 rows.']
    assert speakable('At 138 BPM — `5 ms`.', {'BPM': 'B P M', 'ms': 'milliseconds'}) == 'At 138 B P M, 5 milliseconds.'
