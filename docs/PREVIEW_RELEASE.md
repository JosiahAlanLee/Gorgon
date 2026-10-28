# Gorgon preview-1

Coordinated preview of the Gorgon tracker and VCV ecosystem. Each platform zip
contains nine Rack plugins, a pinned HydraPy wheel and source distribution,
two visual starter patches, the ByteBus example, corresponding source archives for the two OS adapters,
and `INSTALL.txt`. Pins: HydraPy `789faed` (0.1.0), DSP_Emulation `a9f2d2f`.
All four jobs in the [Monomodule platform rerun](https://github.com/JosiahAlanLee/DSP_Emulation/actions/runs/40720062439)
are green. Its earlier ARM job was delayed by GitHub runner capacity.
DrumLab is retired and excluded from this preview.

| Plugin | Version | Includes |
| --- | --- | --- |
| GorgonBridge | 2.4.2 | Six-lane v3 tracker bridge, BUS I/O and Ping |
| OtisLab | 2.11.6 | OTIS Sampler, Kit, Capture and patch selection |
| VastishLab | 2.8.2 | Synthesizer, audio input and triple chain |
| SpoonBenderLab | 2.2.1 | Echo, reverb and sample capture |
| SeanceLab | 2.1.1 | Spectral core and five expanders, including Bridge |
| ByteBusLab | 2.16.5 | Converter, Register, Lookup, counters, routing and NLC bus modules |
| MachinemoduleRack | 2.2.2 | Machinedrum OS adapter, six-lane sound link |
| MonomoduleRack | 2.0.4 | Monomachine OS adapter, six-lane sound link |
| CZishLab | 2.1.3 | CZish phase-distortion voice and DAC Stage |

## Installation

Download the zip for Linux x64, Windows x64, macOS Apple Silicon or macOS Intel.
Follow its `INSTALL.txt` to install the Rack plugins and create a Python virtual
environment. Python 3.9+ and VCV Rack 2 are installed separately. The Python
installation needs internet access for dependencies. JACK/PipeWire is optional
and installed separately; no audio device or hardware route is configured by
the installer. `RELEASE.json` records every source commit and plugin version;
`SHA256SUMS` covers every payload file.

Machinemodule and Monomodule require the user’s own compatible OS images,
loaded through their menus. No OS image or firmware is included. CZish’s
original example SysEx patches are patch data, not firmware.

On Linux, install the optional `gorgon-live` launcher separately and configure
a profile, then use `gorgon start`; see [the launcher manual](LAUNCHER.md).
The launcher is Linux-only. Windows/macOS use the manual Studio instructions
in `INSTALL.txt`.

The tracker still runs in HydraPy Studio with a browser editor. Extracting its
Python core into this repository and adding a native desktop interface are
later changes. This preview does not add a Strudel engine or RAVE host.

## Validation and limits

Publication requires the pinned HydraPy conformance fixture, real Studio
integration, sanitizers and Rack API checks to pass, plus all four platform
builds. A dry-run workflow keeps unresolved blockers in `release.json` and
creates artifacts only. Publication refuses those blockers and creates a
**draft** pre-release for final inspection.

Linux software paths have been exercised. Windows/macOS builds have not been
played inside Rack. Physical interfaces, the ES-40 digital path and end-to-end
audio/display latency require workstation checks. Browser audition audio is
independent of native Breaks audio and is not included in exports.

## Maintainer procedure

1. Fetch HydraPy main. In an isolated checkout, build it with `python -m build`.
   Generate both fixtures with `rack/tools/make_fixture.py` (default and `--v3`)
   using that checkout on `PYTHONPATH`, and compare against `fixture.txt` and
   `fixture3.txt`. Run the portable Rack tests and `tools/check_schema.py`.
   Move `hydrapy.pin` only if both fixtures are unchanged; a difference needs
   the spec-first procedure in [AGENTS.md](../AGENTS.md).
2. Fetch DSP_Emulation main and inspect its Linux and platform CI. Pin the
   tested commit in `release.json`; if a check is red, use the newest fully
   green commit. Queued checks remain a publication blocker until green.
   Update the version table from each pinned `plugin.json`. The default set
   is every active `vcv/*/plugin.json`, plus GorgonBridge; `shared` and `adapter` are
   libraries; the explicit retired set excludes DrumLab. New plugins still
   require matching bundler and workflow updates.
3. Build all nine plugins with their own `make dist` targets. The two OS
   adapters fetch pinned open-source cores, test without firmware, and produce
   corresponding source archives. Use native Intel and ARM macOS runners,
   matching the adapters’ own CI. Do not pass an OS path to any build or test.
   The assembler restricts adapter binary payloads to metadata, libraries,
   documentation and panel assets, and rejects firmware file extensions (including `.zip`, `.img` and `.hex`)
   in their source archives. Seven exact upstream wxWidgets documentation/test
   ZIP paths are allowed only when their bytes match the pinned dependency
   revision. It validates the wheel’s complete package set and
   bytes against the pinned checkout and checks required sdist source and bytes.
   The package-content rule mirrors HydraPy’s explicit setuptools package-data
   declarations; review it whenever the pin’s packaging configuration changes.
4. Run **Gorgon preview bundles** with `publish` unchecked. Inspect all four
   artifacts: nine `.vcvplugin` files, two corresponding-source archives, wheel,
   sdist, two patches, example, install text, release manifest and checksums.
   Resolve each `blocked_by` entry only after its stated check passes.
5. Publication is Josiah’s decision. Run with `publish` checked, or push the
   exact `preview-1` tag. The workflow repeats its checks and creates a draft
   pre-release. Inspect the draft before publishing. Existing releases are
   never overwritten.

Local assembly, after building the nine plugins with `make dist` and building
the pinned HydraPy with `python -m build`:

```sh
python -m unittest discover -s tests -p 'test_release_bundle.py'
python tools/release_bundle.py --check
python tools/release_bundle.py --dsp /path/to/pinned-DSP --hydrapy /path/to/pinned-hydrapy \
  --python-dist /path/to/python-dist --arch lin-x64
```
