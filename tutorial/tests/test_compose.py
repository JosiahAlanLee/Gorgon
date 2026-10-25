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
