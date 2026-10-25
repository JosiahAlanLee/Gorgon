from gtut import config, rack


def test_previous_log_is_set_aside(tmp_path):
    p = tmp_path / 'config.yaml'
    p.write_text(f'paths:\n  work: {tmp_path / "work"}\n')
    cfg = config.load(p)
    assert rack.set_aside_log(cfg) is None          # first launch: nothing to move
    log = rack.user_dir(cfg) / 'log.txt'
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text('[0.1 info] Loading patch\n')    # killed: no END line, Rack would ask about a crash
    prev = rack.set_aside_log(cfg)
    assert not log.exists() and prev.read_text().startswith('[0.1 info]')
