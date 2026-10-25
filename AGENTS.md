# Working on Gorgon

For people and coding agents alike. Gorgon spans two repositories, and most
mistakes come from changing one side without the other. This page is the
routing table.

## Where a change goes

| You are changing | Repository | Notes |
| --- | --- | --- |
| The score format, bridge API, signal semantics or audio port layout | **Gorgon `spec/`**, first | Then the implementations, below. Record it in `spec/CHANGELOG.md`. |
| The tracker editor, Studio, scenes, direct CV, ES-40, JACK audio, patches | [hydrapy](https://github.com/JosiahAlanLee/hydrapy) | Docs in `docs/gorgon-*.md` and `docs/es40.md`. |
| The Rack module: engine, bridge client, panel, manual | **Gorgon `rack/`** | Regenerate the fixture if semantics changed. |
| The pinned HydraPy commit | **Gorgon `hydrapy.pin`** | One file. The weekly workflow proposes moves; see below. |
| OTIS, Vastish, Spoon Bender | [DSP_Emulation](https://github.com/JosiahAlanLee/DSP_Emulation) | Not Gorgon. |

A feature that crosses the boundary (a new signal, a new score field a follower
reads, a new endpoint) is three pull requests in this order:

1. **Gorgon:** `spec/` and `spec/CHANGELOG.md`. Say what each implementation
   must do. Nothing else in this PR.
2. **hydrapy:** the Python side, with tests. When merged, note the commit.
3. **Gorgon:** `rack/` to match, `hydrapy.pin` moved to that commit, and
   `rack/tests/fixture.txt` regenerated from it in the same PR.

A feature that stays on one side (editor behaviour, a Rack context-menu option
whose default matches the contract, a panel change) is one PR in that repository.

## Rules that CI enforces

- `rack/tests/fixture.txt` must be byte-identical to what the pinned HydraPy's
  `TrackerCV` produces. If it isn't, either the pin is wrong for this `rack/`
  or signal semantics changed without going through `spec/`.
- `spec/score.schema.json` must accept every score HydraPy's `validate()`
  accepts and reject what it rejects where a schema can (`tools/check_schema.py`).
- The Rack plugin version in `rack/plugin.json` starts with `2.`.

## Moving the pin

`hydrapy.pin` holds one HydraPy commit SHA. The Linux workflow checks out
HydraPy at that commit, installs it, regenerates the fixture and runs Studio
for the end-to-end test.

- **Automatically:** `hydrapy-main.yml` runs weekly (and on demand) against
  HydraPy `main`. When everything passes and the fixture is unchanged, it opens
  a pull request moving the pin. Merge it. If it fails on the fixture, a
  semantic change landed in HydraPy without a contract change: fix that through
  the three-PR sequence rather than regenerating the fixture to make CI green.
  (The PR step needs *Allow GitHub Actions to create and approve pull requests*
  enabled in this repository's Actions settings.)
- **By hand:** edit `hydrapy.pin`, run `python rack/tools/make_fixture.py >
  rack/tests/fixture.txt` with that HydraPy installed, and commit both if the
  fixture changed along with the `spec/` change that justifies it.

## Running the checks locally

```sh
# Rack module, no Rack SDK needed (jansson required for bridge_test)
make -C rack/tests test
make -C rack/tests sanitize

# Against a real HydraPy Studio (HydraPy installed from the pinned commit)
pip install -e /path/to/hydrapy
make -C rack/tests e2e
python rack/tools/make_fixture.py | cmp - rack/tests/fixture.txt
python tools/check_schema.py

# The plugin itself
make -C rack RACK_DIR=/path/to/Rack-SDK dist
```

## Pull requests

- One concern per PR. Spec, HydraPy and `rack/` changes do not share a PR,
  except pin + fixture + `rack/` in step 3 above.
- Say what was run, and what was not. Hardware, display latency and the
  physical digital path are never validated in CI; say so rather than implying
  otherwise.
- Merge, don't squash: `rack/` carries the module's history from
  DSP_Emulation as a subtree, and squashing flattens it.
- Review your own side against the other's tests: a change to `TrackerCV`
  should be run against `rack/tests/engine_test` before it is proposed, and a
  change to the Rack engine against a regenerated fixture.

## What this repository is not

It is not where the Python tracker lives, yet. Extracting it from HydraPy into
a `gorgon` package that depends on HydraPy is planned for after the current
feature work settles, once Studio exposes the hooks it needs (session lifecycle,
HTTP routes, renderer prewarm). Until then, HydraPy is the reference
implementation and `spec/` is written from it.
