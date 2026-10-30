# Scores

`demo.json` is HydraPy's `hydrapy/tracker/demo.json`, unchanged from `67d0897` to `08a6d8a` (the Gorgon pin). Chapters 4-8 load it in setup so every shot starts from the same score: Studio keeps edits between shots otherwise.

`kit.json` is a six-lane `hydrapy-tracker-3` score built from `demo.json`: lanes 1–4 are rewritten as a drum kit for DrumLab's Percussion: kick, snare, hat and FM (its default four-lane mapping), around C4, which DrumLab plays at each model's base pitch. Lanes 5 and 6 are empty. The picture automation is demo.json's. The cold open (chapter 0) and chapter 10's kit shot load it. `python scores/make_kit.py` rebuilds it.

`kit-md.json` is `kit.json` as a six-lane `hydrapy-tracker-3` score for Machinemodule on Gorgon's sound link (chapter 10): the same hits and picture, with track sounds from the Machinedrum OS 1.63 (TRX kick, snare and closed hat, an FM tom) and per-hit locks that make one-hit changes audible: open hats by machine lock, ghost notes, and the FM figure tuned through P1. `python scores/make_kit_md.py` rebuilds it from `kit.json`.
