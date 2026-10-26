# Scores

`demo.json` is HydraPy's `hydrapy/tracker/demo.json`, unchanged from `67d0897` to `08a6d8a` (the Gorgon pin). Chapters 4-8 load it in setup so every shot starts from the same score: Studio keeps edits between shots otherwise.

`kit.json` is `demo.json` with the four audio lanes rewritten as a drum kit for DrumLab's Percussion: kick, snare, hat and FM (its default four-lane mapping), around C4, which DrumLab plays at each model's base pitch. The picture automation is demo.json's. The cold open (chapter 0) and chapter 10's kit shot load it. `python scores/make_kit.py` rebuilds it.
