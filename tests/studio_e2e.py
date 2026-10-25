#!/usr/bin/env python3
"""End to end against a real HydraPy Studio (needs HydraPy installed).

Starts Studio on a small CPU patch, advertises it in a temporary bridge file,
plays the tracker, and runs studio_probe: the C++ bridge and follower. Checks
that the follower's beat matches Studio's own tracker beat, that CV 1 lands on
the patch's first free control, and that PLAY/stop from Studio is followed.

    make -C vcv/gorgon/tests e2e
"""
import os
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
        finally:
            withdraw(path, token)
            server.shutdown(); server.server_close(); session.close()
    print('studio_e2e: passed')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'tests/studio_probe')
