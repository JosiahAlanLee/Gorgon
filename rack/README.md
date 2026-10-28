# Gorgon for VCV Rack

The Rack 2 module (plugin slug `GorgonBridge`) that links Rack to the Gorgon tracker in [HydraPy](https://github.com/JosiahAlanLee/hydrapy) Studio. Gorgon is the master clock: its clock, reset, gates, held 1 V/oct pitches and automation lanes come out of the module, evaluated per sample, and CV patched in goes back into the visuals' controls and transport.

See the [manual](docs/MANUAL.md). The plugin also carries **Ping**, a latency meter: it times a destination
from its trigger to its sound and stores the result with the patch ([manual](docs/MANUAL.md#ping-measuring-latency)).

Requires a HydraPy Studio with the bridge endpoint (`GET /api/bridge`), on the same computer as Rack. The protocol it speaks is specified in [`../spec`](../spec/README.md); the module's own documentation is the [manual](docs/MANUAL.md).

## Layout

| Path | Contents |
|---|---|
| `src/engine/GorgonEngine.hpp` | Score, playhead, clock follower and output evaluation. No Rack dependency. |
| `src/engine/ScoreV3.hpp` | Score format 3: the score, its timeline and `evaluate()` ([evaluator.md](../spec/evaluator.md)). Allocates nothing. |
| `src/engine/EngineV3.hpp` | Format 3's signals: `evaluate()`'s candidates played one sample at a time, six lanes. |
| `src/engine/BusIn.hpp`, `ByteBusInput.hpp` | BUS IN: lane mutes and FILL read from a byte bus, stamped, and sent to Studio as windows ([bus-io.md](../spec/bus-io.md)). |
| `src/engine/Trace.hpp` | The event trace: a bounded ring of evaluated candidates and BUS IN changes, saved from the menu as JSONL ([evaluator.md](../spec/evaluator.md#trace)). |
| `src/engine/ByteBusOutput.hpp` | Clocked byte encoding of existing lanes and hit state; nine-channel ByteBus output. |
| `src/engine/Bridge.hpp`, `LoopbackHttp.hpp`, `BridgeJson.hpp` | The link to Studio: discovery, a small loopback HTTP client, parsing. |
| `src/Gorgon.cpp` | The Rack module and widget. |
| `src/Ping.cpp`, `src/engine/PingMeter.hpp` | Ping, the latency meter; its measurement has no Rack dependency. |
| `src/AudioInput.hpp`, `src/engine/PcmQueue.hpp` | Four-channel audio capture, sample-rate conversion and bounded PCM transfer. |
| `tools/make_panel.py` | Generates `res/Gorgon.svg` and `src/Layout.hpp`, and `res/Ping.svg` and `src/PingLayout.hpp`, each pair from one table. |
| `tools/make_fixture.py` | Generates `tests/fixture.txt`, the conformance fixture, from HydraPy's own `TrackerCV`, and with `--v3` `tests/fixture3.txt` from its `tracker_v3.evaluate()`. |
| `glyphs/` | The panel mark and the tool that places it. |

## Tests

From the repository root:

~~~sh
make -C rack/tests test        # engine against HydraPy's outputs; bridge against a fake Studio
make -C rack/tests sanitize    # the same under ThreadSanitizer, ASan and UBSan
make -C rack/tests e2e         # against a real HydraPy Studio (needs HydraPy installed)
make -C rack/tests module_test RACK_DIR=/absolute/path/to/Rack-SDK && rack/tests/module_test
make -C rack/tests ping_module_test RACK_DIR=/absolute/path/to/Rack-SDK && rack/tests/ping_module_test
~~~

The portable tests need jansson (`libjansson-dev`). `engine_test` compares the C++ engine with HydraPy across eight random scores, including row and pulse boundaries, pitch/key mapping, AIR and zero velocity. Stateful checks cover held outputs and delayed transport resets; the follower is tested through drift, block-timing jitter, loops and seeks. `evaluator_test` reads `fixture3.txt`, random version-3 scores with HydraPy's candidates and signals: it checks the C++ `evaluate()` candidate by candidate and each candidate's trace line against HydraPy's, then replays the playheads Studio sent and checks the version-3 engine against `TrackerCV` on every sample; it also plays a cycle shorter than half a clock period and checks that CLOCK and BUS OUT keep their period through its wraps. `bytebus_test` covers all 256 lane codes, hit masks, wire levels, sample-clock publication and transport discontinuities at 44.1/48/96 kHz. `ping_test` checks Ping's count against synthetic returns, and `ping_module_test` runs Ping in Rack's own engine, with real cables, into destinations of known latency (0–4799 samples, pulsed and triggered).

Linux CI regenerates both fixtures with the HydraPy commit pinned in [`../hydrapy.pin`](../hydrapy.pin), compares them to the checked-in files, and runs the real Studio bridge end to end. A manual `hydrapy_ref` input can check another version, and a weekly run checks HydraPy's `main`. Tests cover discovery, authentication, transport, controls, pitch, pause, seek and reset pulses, plus concurrent PCM channel ordering and Rack capture at 44.1/48/96 kHz. Physical audio/video latency still needs workstation testing.
