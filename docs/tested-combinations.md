# Tested revision combinations

This page records revision sets that were exercised together and what the
evidence actually establishes. It is not a compatibility promise or a list of
current `main` branches. A blank or "not recorded" entry means the preserved
evidence does not identify that repository revision; a runtime plugin version
is not a source commit.

| Date | Work | Gorgon | HydraPy | DSP_Emulation | Generative_Art | Validation and evidence |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-10-28 | Preview-1 manifest | `d585b0d` | `789faed` | `a9f2d2f` | Not used | CI and package validation, not a rendered performance. [`release.json`](../release.json) pins the DSP revision; [preview procedure](PREVIEW_RELEASE.md) records the HydraPy pin and reports all four Monomodule platform jobs green. Publication status is separate. |
| 2026-10-30 | Namagiri suite film | Runtime `GorgonBridge` 2.4.2; source SHA not recorded | `804420f` | Source SHA not recorded; runtime `OtisLab` 2.12.0, `ByteBusLab` 2.18.0, `SeanceLab` 2.1.1, `MachinemoduleRack` 2.2.3, `VastishLab` 2.8.2, `MonomoduleRack` 2.1.0, `CZishLab` 2.1.3, `SpoonBenderLab` 2.2.1 | Not recorded | Integrated 192 s headless render: zero xruns or discontinuities, full decode, all 14 chapter sheets reviewed; no listening review. Evidence: `shared/codex/namagiri-suite-20261029/attempt02/completion.json`, `runtime.json`, and `take/rack-user/plugins-lin-x64/*/plugin.json`; delivery note: `shared/handoff/2026-10-29-codex-namagiri-suite.md`. |
| 2026-10-30 | Woodland Passage film | Runtime `GorgonBridge` 2.4.2; source SHA not recorded | `804420f` | Source SHA not recorded; same runtime versions as the Namagiri suite | Not recorded | Integrated 120 s headless render: zero xruns or discontinuities, full decode, all eight chapter sheets reviewed; no listening review. Evidence: `shared/codex/woodland-film-20261030/completion.json`, `music/audio-verification.json`, and `audio-take/rack-user/plugins-lin-x64/*/plugin.json`; delivery note: `shared/handoff/2026-10-30-codex-woodland-film.md`. |
| 2026-10-30 | Devstral DV7 film | Runtime `GorgonBridge` 2.4.2; source SHA not recorded | `804420f` | Source SHA not recorded; same runtime versions as the Namagiri suite | Not recorded | Integrated 360 s headless render: zero xruns or clock gaps, six chapter sheets reviewed. Evidence: `shared/codex/devstral-dv7-20261029/performance/completion.json`, `harness-provenance.json`, and `take/rack-user/plugins-lin-x64/*/plugin.json`. |
| 2026-10-30 | Architecture review | `bb42ccb` | `804420f` | `7346b76` | `181c161` | Unit/integration tests only, in isolated copies. Gorgon launcher/capture/latency: 45 passed; HydraPy contract/followers/Studio/Namagiri/diagnostics: 224 passed; five ByteBus C++ programs passed; art subset: 48 passed, 3 skipped, 23 subtests passed. No full GPU, Rack, Blender or physical-device qualification. Evidence: `shared/codex/repository-review-20261030/test-results.json` and logs there; summary: `shared/handoff/2026-10-30-codex-current-codebase-handoff.txt`. |
| 2026-10-30 | HydraPy pin check | `bb42ccb` | Pinned `aff7e46` and candidate `804420f` | Not tested | Not tested | Unit/integration tests and fixture reproduction only. Both HydraPy revisions gave 57 passed, 1 skipped and 3 subtests passed; both generated `fixture.txt` and `fixture3.txt` byte-identically to the checked-in files. `tracker_v3.py` and `studio_tracker.py` are byte-identical between the two revisions. Evidence: `shared/codex/gorgon-pin-check-20261030/`. |
| 2026-10-30 | HydraPy #102 compatibility pin | `1008a08` with `hydrapy.pin` moved to `90af7d0` | `90af7d0` | Not tested | Not tested | Isolated checkout: 62 Gorgon Python tests passed, 1 skipped and 3 subtests passed; all portable Rack and sanitizer suites passed; `fixture.txt`, `fixture3.txt` and `fixture-length3.txt` reproduced byte-identically; real Studio bridge e2e and the headless `prewarm_take` hold/release smoke passed; Rack 2.6.4 module/dist and isolated PipeWire-to-Studio-to-Rack live tests passed. No physical-device qualification. Evidence: `shared/codex/20261030-gorgon-pin-90af7d0/`. |

## HydraPy pin check

The reviewed pin is `aff7e4687e0c274fa9ec6807d5b410dd3ec51813`; HydraPy
`main` at review time was `804420fba42d2782f01514de23338eb44c1d90e3`. The two
files that implement the score contract and the Studio adapter have identical
Git blob hashes at those revisions:

- `hydrapy/tracker_v3.py`: `bb3be86dfc3513a689832400d75497fc2eabb429`
- `hydrapy/studio_tracker.py`: `ee491179650d4f50374a14a8e89a7242b6700b0a`

The later HydraPy revisions contain substantial Studio, renderer and diagnostic
changes, but this check found no score-contract or Studio-adapter drift. A pin
bump therefore looks safe on the tested surfaces. Re-running the fixture tools
is still prudent, but no fixture or Rack semantic regeneration is required if
they remain byte-identical. A real-Studio Rack integration and end-to-end
render remain the appropriate promotion checks; this was a library-level test,
not a release qualification.
