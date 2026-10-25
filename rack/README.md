# Gorgon for VCV Rack

The Rack 2 module (plugin slug `GorgonBridge`) that links Rack to the Gorgon tracker in [HydraPy](https://github.com/JosiahAlanLee/hydrapy) Studio. Gorgon is the master clock: its clock, reset, gates, held 1 V/oct pitches and automation lanes come out of the module, evaluated per sample, and CV patched in goes back into the visuals' controls and transport.

See the [manual](docs/MANUAL.md).

Requires a HydraPy Studio with the bridge endpoint (`GET /api/bridge`), on the same computer as Rack. The protocol it speaks is specified in [`../spec`](../spec/README.md); the module's own documentation is the [manual](docs/MANUAL.md).

## Layout

| Path | Contents |
|---|---|
| `src/engine/GorgonEngine.hpp` | Score, playhead, clock follower and output evaluation. No Rack dependency. |
| `src/engine/Bridge.hpp`, `LoopbackHttp.hpp`, `BridgeJson.hpp` | The link to Studio: discovery, a small loopback HTTP client, parsing. |
| `src/Gorgon.cpp` | The Rack module and widget. |
| `src/AudioInput.hpp`, `src/engine/PcmQueue.hpp` | Four-channel audio capture, sample-rate conversion and bounded PCM transfer. |
| `tools/make_panel.py` | Generates `res/Gorgon.svg` and `src/Layout.hpp` from one table. |
| `tools/make_fixture.py` | Generates `tests/fixture.txt`, the conformance fixture, from HydraPy's own `TrackerCV`. |
| `glyphs/` | The panel mark and the tool that places it. |

## Tests

From the repository root:

~~~sh
make -C rack/tests test        # engine against HydraPy's outputs; bridge against a fake Studio
make -C rack/tests sanitize    # the same under ThreadSanitizer, ASan and UBSan
make -C rack/tests e2e         # against a real HydraPy Studio (needs HydraPy installed)
make -C rack/tests module_test RACK_DIR=/absolute/path/to/Rack-SDK && rack/tests/module_test
~~~

The portable tests need jansson (`libjansson-dev`). `engine_test` compares the C++ engine with HydraPy at 4,192 beats across eight random scores, including row and pulse boundaries, pitch/key mapping, AIR and zero velocity. Stateful checks cover held outputs and delayed transport resets; the follower is tested through drift, block-timing jitter, loops and seeks.

Linux CI regenerates the fixture with the HydraPy commit pinned in [`../hydrapy.pin`](../hydrapy.pin), compares it to the checked-in file, and runs the real Studio bridge end to end. A manual `hydrapy_ref` input can check another version, and a weekly run checks HydraPy's `main`. Tests cover discovery, authentication, transport, controls, pitch, pause, seek and reset pulses, plus concurrent PCM channel ordering and Rack capture at 44.1/48/96 kHz. Physical audio/video latency still needs workstation testing.
