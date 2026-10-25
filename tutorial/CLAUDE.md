# Filming the Gorgon tutorial — brief for Claude Code

You are on Josiah's workstation (Linux, RTX 4090). The job: produce a complete,
narrated, chaptered tutorial video for the Gorgon + HydraPy + VCV Rack modules
ecosystem, recorded entirely inside this computer — no camera, no physical
interface, no modular. This folder (`tutorial/` in the Gorgon repository) is the
kit: a harness (`gtut`), a Rack probe plugin, a draft script, patch builders,
diagrams and a card background.

Read `README.md` here for the layout, `script/STYLE.md` before touching narration,
and `../AGENTS.md` for how changes to this repository go in.

## What "done" means

- `work/final/gorgon-ecosystem-tutorial.mp4` (1080p30, H.264, −16 LUFS, chapter
  markers), its `.srt`, and `.chapters.txt` (YouTube chapter list).
- One MP4 per chapter in `work/chapters/`.
- `work/verify.md`: every `verify:` item in the script, what you checked it
  against (file and commit, or what the stage showed), and the result.
- `work/qc/*.json` with no unexplained problems, and the contact sheets.
- A short report to Josiah: what was recorded at which commits
  (`work/hydrapy.sha`, `work/plugins.json`, Gorgon HEAD), what differs from the
  draft script and why, what you could not check (you can't listen: pacing,
  pronunciation and mix levels need his ears), and anything still rough.

Nothing is published or uploaded. Josiah watches it and decides.

## Rules

1. **The video says only what is true of the build it shows.** Every shot has
   `source:` (the docs it rests on) and `verify:` (claims to check). Check them
   against the pinned HydraPy commit and the plugin sources being built, and on
   the stage itself. When the build and the script disagree, change the
   narration to match the build; never stage around a bug to make the script
   true. When a feature misbehaves, note it in the report and leave it out of
   the video unless Josiah says otherwise.
2. **Never present unvalidated things as working.** The Gorgon README lists what
   is not validated (physical interfaces, the ES-40 path, display/device
   latency, a signed-in Rack Pro). Chapter 14 says so; keep it that way, and
   re-read the README's Status section in case it changed.
3. **Nothing personal on screen or in captions.** The Studio session token (URL
   fragment), home paths, usernames, hostnames, email, other windows. The
   harness hides the token (Chromium app mode; `show_safe` in the terminal; the
   terminal prompt is bare) and QC OCRs frames for it. Treat any QC privacy hit
   as a blocker. Don't film his desktop: the stage is its own X display.
4. **Don't disturb his studio.** The stage has its own X display, its own JACK
   server (`gtut`), its own Rack user folder, its own Studio port and bridge file,
   and a HydraPy *worktree* — his checkout's working tree is never switched.
   Don't kill processes you didn't start (`gtut stage down` stops only the
   registry in `work/stage/procs.json`). If his own JACK/PipeWire session or
   Studio is running, leave it alone.
5. **Ask Josiah before:** anything with `sudo` (apt installs, `nvidia-xorg`
   display mode); choosing the narration voice (a Kokoro voice, or him recording
   with `gtut narrate --prompter`); how HydraPy, Gorgon, Vastish and Séance are
   pronounced (`narration.lexicon`); cutting or adding chapters; changing
   anything outside `tutorial/`; pushing a branch or opening a PR. Everything
   else — iterating takes, fixing selectors, retiming, editing narration to
   match the build — is yours to do.
6. **Changes to the kit go in as one-concern PRs to Gorgon** (see `../AGENTS.md`):
   harness fixes, probe fixes and script edits separately. The Rack probe is a
   filming aid, not part of `GorgonBridge`; don't mix it into `rack/`.

## Phases

Run everything from `tutorial/` inside the venv (`source work/venv/bin/activate`).

### 0. Environment

```sh
./bootstrap.sh            # first run writes config.yaml: set paths, then run again
gtut preflight            # fix every FAIL; WARNs are judgement calls, write down which you accept
```

`paths.rack` must be **VCV Rack Free** 2.x (an unsigned Rack Pro processes no
audio — Gorgon README). `paths.rack_sdk` must match its version. Preflight
checks that Studio's GL context is the 4090 through EGL (not llvmpipe) and that a
JACK client started the way the stage starts them cannot reach any server but the
stage's. Where PipeWire's JACK libraries are the system default (this
workstation), a client ignores `JACK_DEFAULT_SERVER` and joins the desktop session,
and jackd1 cannot start; in `jackd` mode gtut links the real libjack and
libjackserver into `work/stage/libjack`, puts that first on `LD_LIBRARY_PATH` for
jackd and every stage process, and restarts itself under it (`audio.libjack`). The
stage also refuses a server with any ports besides its dummy driver's.

### 1. Build what is filmed

```sh
gtut setup                # HydraPy worktree at Gorgon's hydrapy.pin; build + install the five plugins and the probe
gtut catalog              # starts Rack once on the stage; writes work/catalog.json (ports, params, panel widths)
gtut preflight            # again: now it can confirm Rack Free and the installed plugins
```

Use the pin unless Josiah says otherwise: the pin is what the Rack module is
tested against. Record the DSP_Emulation commit you build (`work/plugins.json`).

### 2. Harvest GUI-only state, then build patches

Some module state is only reliably set from Rack's GUI: the Audio 2 module's JACK
driver and device, and Gorgon's capture device. Set it once and keep the JSON:

```sh
gtut serve &                                  # holds a stage; `gtut do` drives it
gtut patches                                  # builds work/patches/harvest.vcv among others
gtut do '{"rack.load": "harvest"}'
gtut do '{"rack.menu": {"ref": "AudioInterface2", "path": ["JACK"]}}'   # then the device; read item texts:
curl -s -X POST localhost:8791/layout -d '{}' | python -m json.tool | less   # "menu": open items and their centres
gtut do '{"rack.save": "work/harvest-saved.vcv"}'
gtut harvest work/harvest-saved.vcv Core AudioInterface2 audio-jack
gtut harvest work/harvest-saved.vcv GorgonBridge Gorgon gorgon-capture
gtut patches                                  # again, with the harvested blocks
```

Menu item texts are whatever Rack and the modules print — read them from the
layout dump rather than guessing. Studio must have its JACK ports enabled
(`{"studio.api": {"action": "jack_audio", "enabled": true}}`) before Gorgon's
capture device can be chosen. Read every `work/patches/*.cables.txt`: each line is
a cable with the port names it resolved to. If a name in `patches/10-rack.py` is
wrong the build fails and lists what exists; fix the builder.

### 3. Verify the script

For each chapter, check each `verify:` item. Most are answered by the docs and
source at the built commits; some need the stage (`gtut serve`, `gtut do`,
`gtut stage shot FILE.png`, then look at the PNG). Write `work/verify.md` as you
go. Expect to change narration: the draft was written from the docs, not from a
run. Keep the style guide.

### 4. Rehearse every live shot

```yaml
narration: {engine: espeak}   # in config.yaml: instant, robotic, right length
```

```sh
gtut narrate
gtut shoot 04:play             # one shot; prints its QC and contact sheet path
```

Look at the contact sheet (Read the PNG). Typical fixes: a selector that doesn't
match (the take fails with a Playwright error and leaves `failure.png`), an
action that runs late (QC says how late; shorten what precedes it or move the
cue), framing that hides what the narration names, Rack coordinates off because
the view hadn't settled. `gtut serve` + `gtut do` is the fast loop for trying an
action; `gtut shoot --attach` records with the held stage.

Selectors: the tracker's element ids are in `hydrapy/tracker/index.html`, and
working Playwright selectors are in HydraPy's `tests/test_studio_tracker_browser.py`
and `tests/tracker_editor.mjs`. Prefer ids and accessible names.

### 5. The real voice, then compose

Set `narration.engine` to what Josiah chose; `gtut narrate --force`. Cue times
change with the voice, so **reshoot every live shot** after narrating (compose
refuses a take whose narration changed). Then:

```sh
gtut shoot 02 03 04 ...        # or all: gtut shoot 00 01 ... 14
gtut renders                   # offline HydraPy renders (render shots, card backgrounds)
gtut compose 00 01 ...         # segments
gtut chapter 00 01 ...         # chapter files + QC
gtut final                     # the whole tutorial + QC
```

Read every QC report. Check legibility at phone size: scale a contact sheet to
390 px wide and look; zoom framing in (`frames: rect`) where text is too small,
or set `stage.ui_scale: 1.25` and reshoot.

## The stage

```
stage screen 3840×2160 (Xvfb :77 by default)
┌──────────────────────────┬──────────────────────────┐
│ studio: tracker (Chromium│ program: output window   │
│ app mode, 1920×1080)     │                          │
├──────────────────────────┼──────────────────────────┤
│ rack: VCV Rack Free      │ terminal: xterm          │
└──────────────────────────┴──────────────────────────┘
```

A take records only the windows a shot frames, 1:1, with the programme sound
from Rack's Audio module (JACK) and a slate channel. Compose crops and zooms in
post (`frames:` keyframes), so capture once and frame freely.

- **Studio** runs without `DISPLAY` so moderngl uses EGL on the 4090. Its state
  (`.hydrapy/`) lives in the HydraPy worktree, not his checkout.
- **Rack** runs with `-u work/stage/rack-user`. Every generated patch includes a
  `Director` module (the probe), parked far right. The probe writes
  `work/stage/probe/layout.json` (screen coordinates of every module, port, param
  and open menu item, plus Rack's frame rate) and reads
  `work/stage/probe/commands.jsonl`. See `probe/README.md`.
- **Display modes** (`stage.display`): `xvfb` draws Rack with software GL. If QC
  reports Rack's frame rate below the capture rate, install VirtualGL 3 and use
  `xvfb+virtualgl` (Rack under `vglrun -d egl`). `nvidia-xorg` runs a second,
  monitorless X server on the GPU and needs sudo — ask first.
- **Sync.** Each take starts with a slate: a white flash in the filmed window and
  a click on the slate channel, fired together. Compose measures the offset
  between them and aligns the programme sound. Offsets over 200 ms or JACK xruns
  in a take mean the machine was overloaded; find out why before trusting it.

## Script format

`script/NN-slug.yaml`; the docstring at the top of `gtut/script.py` lists every
key, and `gtut actions` lists the action vocabulary. Narration carries cue marks
(`{play}`) and actions wait for them (`at: play`), so the hand moves when the
voice says. `gtut check` validates everything and estimates length.

Rack references: `Gorgon`, `Vastish#2`, `Gorgon.out.Clock`, `Gorgon.in.CV 1`,
`Scope.in#0`, `Gorgon.param.Timing offset` — names are Rack's own port and param
names, matched case-insensitively: exact, then prefix, then substring.

## Things nobody has checked yet

The harness was exercised on a CPU-only container (Xvfb, JACK dummy, Studio on
the CPU backend, Chromium, xterm, ffmpeg x11grab + JACK, compose and QC, the
cards and stills). These were **not** run there, so expect to fix them first:

- the probe plugin (`probe/`): it compiles against the Rack 2.6.4 headers, but
  has never been loaded into Rack. Build it, load `work/patches/blank.vcv`, and
  check `layout.json` coordinates against a screenshot before anything else;
- everything that drives Rack: cable drags, menus, `rack.add` (whether Enter adds
  the browser's top result), knob ramps, the view animation;
- Fundamental and Core port names and indices used in `patches/10-rack.py`
  (Scope, Split, LFO, VCMixer, Audio 2) — check the `.cables.txt` files;
- Rack's JACK client and port names (`audio.record: auto` looks for "Rack" or
  "RtApi" in the client name; with a stage up, `JACK_DEFAULT_SERVER=gtut jack_lsp` shows the truth);
- NVENC, EGL, Kokoro and faster-whisper (none on that machine);
- the scene column's accessible name (assumed "Scene, row NN"), the `+12`
  button's name, `#duplicateTarget`'s option values.

## Where things are when something fails

- `work/takes/<shot>/<time>/take.json` — action log with lateness, error trace,
  probe log, Rack fps; `failure.png` beside it on error; `contact.png`.
- `work/stage/logs/*.log` — Xvfb, openbox, jack, studio, rack, terminal.
- `work/stage/rack-user/log.txt` — Rack's own log (plugin load errors).
- `work/stage/probe/layout.json` — where Rack thinks everything is right now.
