# Gorgon

A bridge between VCV Rack 2 and the Gorgon tracker in [HydraPy](https://github.com/JosiahAlanLee/hydrapy) Studio. Gorgon is the master clock: its clock, reset, gates, held 1 V/oct pitches and automation lanes come out of the module, evaluated per sample, and CV patched in goes back into the visuals' controls and transport.

See the [manual](docs/MANUAL.md).

Requires a HydraPy Studio with the bridge endpoint (`GET /api/bridge`), on the same computer as Rack.

## Layout

| Path | Contents |
|---|---|
| `src/engine/GorgonEngine.hpp` | Score, playhead, clock follower and output evaluation. No Rack dependency. |
| `src/engine/Bridge.hpp`, `LoopbackHttp.hpp`, `BridgeJson.hpp` | The link to Studio: discovery, a small loopback HTTP client, parsing. |
| `src/Gorgon.cpp` | The Rack module and widget. |
| `src/AudioInput.hpp`, `src/engine/PcmQueue.hpp` | Four-channel audio capture, sample-rate conversion and bounded PCM transfer. |
| `tools/make_panel.py` | Generates `res/Gorgon.svg` and `src/Layout.hpp` from one table. |
| `tools/make_fixture.py` | Generates `tests/fixture.txt` from HydraPy's own `TrackerCV`. |

## Tests

From the repository root:

~~~sh
make -C vcv/gorgon/tests test        # engine against HydraPy's outputs; bridge against a fake Studio
make -C vcv/gorgon/tests sanitize    # the same under ThreadSanitizer, ASan and UBSan
make -C vcv/gorgon/tests e2e         # against a real HydraPy Studio (needs HydraPy installed)
make -C vcv/gorgon/tests module_test RACK_DIR=/absolute/path/to/Rack-SDK && vcv/gorgon/tests/module_test
~~~

The portable tests need jansson (`libjansson-dev`). `engine_test` compares the C++ engine with HydraPy at 4,192 beats across eight random scores, including row and pulse boundaries, pitch/key mapping, AIR and zero velocity. Stateful checks cover held outputs and delayed transport resets; the follower is tested through drift, block-timing jitter, loops and seeks.

Linux CI regenerates the fixture with a pinned HydraPy commit, compares it to the checked-in file, and runs the real Studio bridge end to end. A manual `hydrapy_ref` input can check another version. Tests cover discovery, authentication, transport, controls, pitch, pause, seek and reset pulses, plus concurrent PCM channel ordering and Rack capture at 44.1/48/96 kHz. Physical audio/video latency still needs workstation testing.
