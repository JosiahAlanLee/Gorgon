# Gorgon preview-1

Coordinated preview of the Gorgon tracker and VCV ecosystem. Each platform zip
contains all six Rack plugins, a pinned HydraPy wheel and source distribution,
two visual starter patches, the ByteBus example, and `INSTALL.txt`.

| Plugin | Version | Includes |
| --- | --- | --- |
| GorgonBridge | 2.2.1 | Tracker bridge, audio capture and BUS OUT |
| OtisLab | 2.8.0 | OTIS Sampler, frame-bus selection and table export |
| VastishLab | 2.5.0 | Synthesizer, FUN logic and bus processing |
| SpoonBenderLab | 2.2.0 | Echo, reverb and sample capture |
| SeanceLab | 2.1.0 | Spectral core and five expanders, including Bridge |
| ByteBusLab | 2.2.0 | Converter, Clocked Register and Lookup |

## Installation

Download the zip for Linux x64, Windows x64, macOS Apple Silicon or macOS Intel.
Follow its `INSTALL.txt` to install the Rack plugins and create a Python virtual
environment. Python 3.9+ and VCV Rack 2 are installed separately. The Python
installation needs internet access for dependencies. JACK/PipeWire is optional
and installed separately; no audio device or hardware route is configured by
the installer. `RELEASE.json` records every source commit and plugin version;
`SHA256SUMS` covers every payload file.

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

1. Merge the remaining short-seek and equivalent-note-spelling fixes. Move
   `hydrapy.pin` to the tested HydraPy commit and regenerate/compare the fixture.
   The pin used during preparation includes the invalid-key fix and must be
   available on GitHub before CI can check it out.
2. Set `dsp_commit` to the tested DSP_Emulation commit in `release.json`. Update
   this version table if any plugin version changes. Remove each `blocked_by`
   entry only after its fix and regression tests land.
3. Run **Gorgon preview bundles** with `publish` unchecked. Inspect all four
   artifacts. Every zip must contain six `.vcvplugin` files and the tracker
   assets in its wheel.
4. Run with `publish` checked, or push the exact `preview-1` tag. The workflow
   repeats its checks and creates a draft pre-release. Inspect the draft and
   publish it when ready. Existing releases are never overwritten.

Local assembly, after building the six plugins with `make dist` and building
the pinned HydraPy with `python -m build`:

```sh
python -m unittest discover -s tests -p 'test_release_bundle.py'
python tools/release_bundle.py --check
python tools/release_bundle.py --dsp ../DSP_Emulation --hydrapy ../hydrapy \
  --python-dist /path/to/python-dist --arch lin-x64
```
