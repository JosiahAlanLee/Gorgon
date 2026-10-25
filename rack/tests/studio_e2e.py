#!/usr/bin/env python3
"""End to end against a real HydraPy Studio (needs HydraPy installed).

Starts Studio on a small CPU patch, advertises it in a temporary bridge file,
plays the tracker, and runs studio_probe: the C++ bridge and follower. Checks
that the follower's beat matches Studio's own tracker beat, that CV 1 lands on
the patch's first free control, and that PLAY/stop from Studio is followed.

    make -C vcv/gorgon/tests e2e
"""
import os
import copy
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from hydrapy.studio import Session, advertise, make_server, withdraw

PATCH = ("from hydrapy import *\n"
         "z = Param('Dolly Z', .35)\nroll = Param('Roll', 0, min=-45, max=45)\nwarp = Param('Warp', 0, min=-2, max=2)\n"
         "solid(z, 0, 0).out(o0)\n")


def check_outputs(session, probe):
    """The real API/parser/follower/output path, including cached transport updates."""
    session.call('tracker_transport', enabled=True, playing=False, beat=0)
    document = copy.deepcopy(session.tracker.document)
    document.update(bpm=120, key=7, loop=False, pattern=0)
    for pattern in document['patterns']:
        pattern['audio'] = [[None] * 64 for _ in range(4)]
    for row, (note, velocity) in enumerate([('C-4', 100), ('C-5', 100), ('AIR', 80), ('D-5', 0)]):
        document['patterns'][0]['audio'][0][row] = dict(note=note, vel=velocity)
    session.call('tracker_score', document=document, revision=session.tracker.revision)
    # 20 ms lead skips the phase reset window; C3 is the chosen pitch reference.
    proc = subprocess.Popen([probe, '3', '7.5', '20', '48'], stdout=subprocess.PIPE, text=True)
    phase, seen = 0, set()
    final_reset_samples = None
    try:
        for line in proc.stdout:
            parts = line.split()
            if parts[0] == 'reset_samples':
                final_reset_samples = int(parts[1])
            if parts[0] != 'out':
                continue
            beat, linked, moving, gate, vel, pitch, revision, edges = map(float, parts[1:])
            if not linked:
                continue
            if phase == 0:
                session.call('tracker_transport', playing=True, beat=0)
                phase = 1
            elif phase == 1 and moving:
                if beat < .25:
                    assert abs(pitch - 17/12) < 1e-5, (beat, pitch)
                    seen.add('C4')
                elif beat < .5:
                    assert abs(pitch - 29/12) < 1e-5, (beat, pitch)
                    seen.add('C5')
                elif beat < .75:
                    assert abs(pitch - 29/12) < 1e-5, (beat, pitch)
                    seen.add('AIR')
                elif beat < 1:
                    assert gate == 0 and abs(pitch - 29/12) < 1e-5, (beat, gate, pitch)
                    assert abs(vel - 80/127) < 1e-5, vel
                    seen.add('silent')
                else:
                    assert edges == 1, edges
                    session.call('tracker_transport', playing=False)
                    phase = 2
            elif phase == 2 and not moving:
                assert gate == 0 and abs(pitch - 29/12) < 1e-5, (gate, pitch)
                seen.add('pause')
                session.call('tracker_transport', playing=True, beat=8)
                phase = 3
            elif phase == 3 and moving and beat >= 8:
                assert pitch == 0 and vel == 0 and edges == 2, (pitch, vel, edges)
                seen.add('seek')
                # Editing the score must not re-latch RESET.
                document['bpm'] = 121
                session.call('tracker_score', document=document, revision=session.tracker.revision)
                phase = 4
            elif phase == 4:
                assert edges == 2, edges
        assert proc.wait(timeout=5) == 0
        assert seen == {'C4', 'C5', 'AIR', 'silent', 'pause', 'seek'}, seen
        assert final_reset_samples == 480, final_reset_samples  # two full 5 ms pulses
    finally:
        if proc.poll() is None:
            proc.kill(); proc.wait()
    print('real Studio outputs: pitch, key/reference, AIR, velocity zero, pause, seek and latched resets passed')


def main(probe):
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        (root / 'p.py').write_text(PATCH)
        os.environ['HYDRAPY_STUDIO_FILE'] = str(root / 'studio.json')
        session = Session(root, 'p.py', backend='cpu', size=(32, 18), fps=30)
        server, token = make_server(session, 0)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        path = advertise(server.server_port, token, root)
        try:
            session.call('tracker_transport', enabled=True, playing=True, beat=0)
            out = subprocess.run([probe, '4', '7.5'], capture_output=True, text=True, timeout=30, check=True).stdout
            lines = [l.split() for l in out.splitlines()]
            samples = [(float(t), float(b), int(linked), int(moving), int(jumps))
                       for t, b, linked, moving, jumps in (l for l in lines if len(l) == 5)]
            info = dict(l[:1] + [' '.join(l[1:])] for l in lines if len(l) != 5 and l)
            # Compare with Studio's beat at the same monotonic instants (same machine,
            # CLOCK_MONOTONIC on both sides on Linux).
            errors = []
            for t, beat, linked, moving, _ in samples:
                if not (linked and moving):
                    continue
                with session.lock:
                    s = dict(session.bridge_state)
                clock = s['clock_time'] + (t - s['wall'])
                truth = session.tracker.normalize(s['anchor_beat'] + (clock - s['anchor_time']) * s['document']['bpm'] / 60)
                e = (truth - beat + 32) % 64 - 32
                errors.append(abs(e) * 60 / s['document']['bpm'] * 1000)
            assert len(errors) > 40, f'too few linked samples: {len(errors)}\n{out}'
            settled = errors[10:]
            print(f'follower vs Studio over {len(settled)} samples: max {max(settled):.2f} ms, '
                  f'median {sorted(settled)[len(settled) // 2]:.2f} ms; jumps {samples[-1][4]}')
            assert max(settled) < 1, f'follower error {max(settled):.2f} ms'
            assert samples[-1][4] <= 2, f'{samples[-1][4]} jumps while playing steadily'
            warp = session.renderer.patch.controls['Warp'].base
            assert info.get('target0') == 'Warp', info
            assert abs(warp - 1.0) < 1e-6, f'CV 1 at 7.5 V should set Warp to 1.0, got {warp}'
            assert info['status'].startswith('Linked'), info
            print('CV 1 7.5 V -> Warp', warp, '| status:', info['status'])
            check_outputs(session, probe)
        finally:
            withdraw(path, token)
            server.shutdown(); server.server_close(); session.close()
    print('studio_e2e: passed')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'tests/studio_probe')
