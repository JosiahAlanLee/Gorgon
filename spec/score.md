# The score

A score is one JSON document, format `hydrapy-tracker-1`. The machine-readable
shape is [score.schema.json](score.schema.json); this page says what the fields
mean. The reference validator is `validate()` in HydraPy's `studio_tracker.py`.

## Shape

```
format     "hydrapy-tracker-1"
bpm        60–200                 quarter notes per minute; a row is a sixteenth
key        0 | 2 | 5 | 7          root of the key: C, D, F or G
pattern    0–3                    the selected pattern
loop       bool                   cycle the selected pattern (16 beats) or all four (64)
sound      bool                   browser audition synth; editor state only
mutes      [bool ×4]              one per audio lane
modes      {program, aux}         which output (o0–o3) each monitor shows
bindings   {z, roll, feedback}    patch control name each automation lane drives; "" disconnects
patterns   [pattern ×4]
```

A pattern:

```
name       1–40 characters
audio      [lane ×4], each [cell ×64]; a cell is null or {note, vel}
keys       {z: [{r, v}…], roll: […], feedback: […]}   sorted by r, unique r, ≤64 per lane
easing     {z, roll, feedback}  each hold | linear | smooth
scenes     [{r, name, fade}…]   optional, default []; sorted by r, unique r, ≤64
```

Unknown keys at the top level and inside a pattern are tolerated and preserved.
A scene change must have exactly `r`, `name` and `fade` (`fade` defaults to 0 on
load). Non-finite numbers are rejected everywhere.

## Time

The playhead is a **beat** in quarter notes. The cycle is either the selected
pattern, beats `[16·pattern, 16·pattern + 16)`, when `loop` is true, or the whole
score, `[0, 64)`, when it is false. Beats wrap within the cycle.

- pattern index = ⌊beat / 16⌋
- row within the pattern = ⌊(beat mod 16) · 4⌋, 0–63
- row phase = fractional part of (beat mod 16) · 4, used for gate length

At tempo `bpm`, beat advances at `bpm / 60` per second from an anchor:
`beat(t) = anchor_beat + (t − anchor_time) · bpm / 60`, wrapped into the cycle.
A tempo change keeps the current beat and re-anchors. The playhead is owned by
Studio; see [bridge-api.md](bridge-api.md) for how followers read it.

## Notes

`note` is `C0`–`G9` with an optional separator or sharp: `C-4`, `C4` are both
MIDI 60 (tracker notation uses the separator so `C-4` is not "C minus 4");
`D#4` is 63. `B#` and `E#` are rejected, as is anything above MIDI 127.
`AIR` is an unpitched event: it behaves as a hit for gate and velocity, and
leaves the held pitch unchanged.

Pitched notes are transposed by `key − 2` semitones: the reference key is D
(2), so a score in D plays its notes as written.

`vel` is 0–127. Zero is silent: no gate, no hit, and held pitch and velocity do
not change. A muted lane behaves as if every cell were silent.

## Automation lanes

| Lane | Range | Default | Direct CV | Rack (10 V profile) |
| --- | --- | --- | --- | --- |
| `z` | 0–1 | 0.35 | 0–5 V | 0–10 V |
| `roll` | −45–45 ° | 0 | −5–5 V | −5–5 V |
| `feedback` | 0–0.9 | 0.25 | 0–5 V | 0–10 V |

The value at a fractional row `x` within a pattern:

- no keys: the lane default
- `x ≤` first key's row: the first key's value
- after the last key: the last key's value
- between keys `a` and `b`: `a.v + (b.v − a.v) · f(t)` with `t = (x − a.r) / (b.r − a.r)`
  and `f` = 0 for `hold`, `t` for `linear`, `t·t·(3 − 2t)` for `smooth`

Lanes are evaluated per pattern: the last key of one pattern does not
interpolate into the first key of the next.

## Scenes

Scenes are visual: a patch declares named chains per output and the score picks
one per row. CV followers ignore the `scenes` field entirely.

- Changes are collected across the playing cycle in order. Each output follows
  only the changes that name one of its own scenes.
- A change holds until the next change for that output, wrapping round the
  cycle. An output with no change anywhere in the cycle shows its first declared
  scene.
- `fade` crossfades from the previous scene over that many rows, smoothstepped.
  It is clipped to the gap before the next change, and a change to the scene
  already showing does not fade.
- A name the patch doesn't declare is kept in the score and ignored when playing.
- A seek, loop change or pause does not clear the renderer's feedback history;
  every scene draws into the same output buffer, so feedback carries across a
  change.

The weight arithmetic, including declaration-order handling so a fade is a true
A→B crossfade, is `scene_weights()` in HydraPy's `studio_tracker.py`.

## Revisions

Studio numbers the score. `revision` increments on every committed document,
including Studio's own program-output syncs, so a follower can ask for the
document only when it changed. `transport_revision` increments only on a
discontinuity: an explicit seek, or automation being enabled or disabled.
Ordinary edits, tempo changes and output syncs do not change it. Followers use `transport_revision`, not
`revision`, to decide when to emit a reset pulse and clear held values.
