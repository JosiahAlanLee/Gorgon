from gtut.xinput import Hand


def test_straight_path_stays_on_the_row():
    h = Hand(cfg=None)
    pts, _ = h._path((100, 500), (450, 500), 0.4, straight=True)
    assert pts[-1] == (450, 500) and all(y == 500 for _, y in pts)  # a menu row is ~21 px tall


def test_default_path_arcs_but_ends_on_target():
    h = Hand(cfg=None)
    pts, _ = h._path((100, 500), (450, 500), 0.4)
    assert pts[-1] == (450, 500) and max(abs(y - 500) for _, y in pts) > 0
