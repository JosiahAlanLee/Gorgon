import numpy as np

from gtut.compose import crop, rect_at, smooth


def test_rect_interpolation_eases_between_keys():
    track = [(0.0, [0, 0, 1920, 1080], 0.7), (2.0, [0, 0, 960, 540], 1.0)]
    assert rect_at(track, 1.0) == [0, 0, 1920, 1080]
    assert rect_at(track, 2.5) == [0, 0, 1440, 810]
    assert rect_at(track, 9.0) == [0, 0, 960, 540]
    assert smooth(0.5) == 0.5


def test_crop_pads_outside_the_take():
    f = np.full((100, 200, 3), 255, np.uint8)
    out = crop(f, [-50, -25, 200, 100], (64, 32))
    assert out.shape == (32, 64, 3)
    assert out[0, 0].tolist() == [16, 16, 20] and out[-1, -1].tolist() == [255, 255, 255]


def test_slate_region_is_the_window_that_flashed():
    from gtut.compose import slate_region
    take = {'rect': [0, 0, 1920, 2160], 'slate_rect': [0, 1080, 1920, 1080]}
    assert slate_region(take) == (0.0, 0.5, 1.0, 0.5)
    assert slate_region({'rect': [0, 1080, 1920, 1080], 'slate_rect': [0, 1080, 1920, 1080]}) is None
    cfg = {'stage': {'windows': {'studio': [0, 0, 1920, 1080]}}}
    assert slate_region({'rect': [0, 0, 3840, 2160], 'slate_method': 'studio'}, cfg) == (0.0, 0.0, 0.5, 0.5)


def test_subtitles_split_a_long_caption_into_readable_timed_pieces():
    from gtut.compose import subtitles
    text = ('Gates one to four are the same gates on separate jacks. Z, Roll and Feedback are the visual lanes '
            'as control voltage. The next four jacks carry audio from Studio; chapter thirteen covers them.')
    subs = subtitles([dict(start=10.0, end=30.0, text=text)])
    assert len(subs) > 3
    assert subs[0]['start'] == 10.0 and abs(subs[-1]['end'] - 30.0) < 1e-9
    for a, b in zip(subs, subs[1:]):
        assert abs(a['end'] - b['start']) < 1e-9
    for s in subs:
        assert all(len(line) <= 42 for line in s['text'].split('\n')) and s['text'].count('\n') <= 1
    assert ' '.join(s['text'].replace('\n', ' ') for s in subs) == text


def test_subtitles_keep_short_captions_and_join_short_sentences():
    from gtut.compose import subtitles
    assert subtitles([dict(start=0, end=2, text='Press PLAY on OTIS.')])[0]['text'] == 'Press PLAY on OTIS.'
    assert [s['text'] for s in subtitles([dict(start=0, end=3, text='It plays. Then it stops.')])] == ['It plays. Then it stops.']
