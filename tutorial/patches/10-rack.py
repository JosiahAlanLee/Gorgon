"""Rack patches for chapters 2 and 9-13, built by name from the catalog (gtut patches).

Patches that reuse DSP_Emulation's own examples (OTIS's embedded demo sample, Séance Bridge's feedback
patch) read them from paths.dsp_emulation, so the film shows what the repository ships.

Each returns the patch at the *start* of its shot: cables that are patched on camera
are left out here. Module and port names are Rack's own (configInput/configOutput),
matched case-insensitively, exact then prefix then substring; `#n` picks by index.

GUI-only state comes from patches/harvested/*.json (see CLAUDE.md, "Harvest"):
  audio-jack.json      an Audio 2 module's data on the stage JACK server
  gorgon-capture.json  a Gorgon module's data with Studio's JACK client as capture device
Without them the patches still load, silent.
"""
import json
from pathlib import Path

from gtut.vcvpatch import Patch


def _harvested(d: Path, name: str):
    p = Path(d) / f'{name}.json'
    return json.loads(p.read_text()) if p.exists() else None


def _example(cfg, rel: str):
    """A DSP_Emulation example patch (paths.dsp_emulation), or None without the config or the file."""
    root = cfg.path('dsp_emulation') if cfg else None
    p = root / rel if root else None
    return json.loads(p.read_text()) if p and p.exists() else None


def build(cat, harvested, cfg=None):
    audio = _harvested(harvested, 'audio-jack')
    capture = _harvested(harvested, 'gorgon-capture')
    out = {}

    def base(with_capture=False):
        p = Patch(cat)
        g = p.add('GorgonBridge', 'Gorgon', x=0, data=capture if (with_capture and capture) else None)
        return p, g

    def audio_out(p, y=1):
        return p.add('Core', 'AudioInterface2', x=0, y=y, data=audio)

    # ch2 / ch9: Gorgon alone
    p, g = base()
    out['rack-gorgon'] = p

    # ch9 scope: Gorgon and a scope, unpatched
    p, g = base()
    p.add('Fundamental', 'Scope', gap=2)
    out['rack-scope-start'] = p

    # ch10 one lane into Vastish: Gorgon's pitch and gates already split (on camera only Split -> Vastish is
    # patched; four drags would outrun the narration), Vastish -> Audio
    p, g = base()
    sp = p.add('Fundamental', 'Split', gap=2)
    sg = p.add('Fundamental', 'Split')
    v = p.add('VastishLab', 'Vastish', gap=2)
    a = audio_out(p)
    p.cable(g, 'Pitch', sp, '#0').cable(g, 'Gates 1', sg, '#0')
    p.cable(v, 'Left audio', a, '#0').cable(v, 'Right audio', a, '#1')
    out['rack-voice-start'] = p

    # ch10 all lanes, polyphonic
    p, g = base()
    v = p.add('VastishLab', 'Vastish', gap=2)
    a = audio_out(p)
    p.cable(g, 'Pitch', v, 'Pitch').cable(g, 'Gates 1', v, 'Gate').cable(g, 'Velocity', v, 'Velocity')
    p.cable(v, 'Left audio', a, '#0').cable(v, 'Right audio', a, '#1')
    out['rack-voice-poly'] = p

    # ch0 cold open, ch10 kit: DrumLab's Percussion as a four-lane kit, wired as its Gorgon-percussion.vcv
    # example: Gates -> TRIG, Pitch -> V/OCT, Velocity -> VEL, Reset -> RESET, stereo mix -> Audio
    p, g = base()
    k = p.add('DrumLab', 'Percussion', gap=2)
    a = audio_out(p)
    p.cable(g, 'Gates 1', k, 'Triggers').cable(g, 'Pitch', k, 'Pitch').cable(g, 'Velocity', k, 'Velocity')
    p.cable(g, 'Reset', k, 'Reset')
    p.cable(k, 'Stereo mix left', a, '#0').cable(k, 'Stereo mix right', a, '#1')
    out['rack-kit'] = p

    # ch10 sound locks: Machinemodule touching Gorgon's right side, TRIG unpatched, so it takes Gorgon's sound link;
    # its native tempo matches kit-md.json. The OS is the user's own (paths.machinedrum_os); without it the patch
    # loads and stays silent.
    os_file = cfg.path('machinedrum_os') if cfg else None
    p, g = base()
    m = p.add('MachinemoduleRack', 'Machinemodule', params={'Engine tempo for synced retrigs/LFOs': 138},
              data=dict(firmwarePath=str(os_file), samplePath='') if os_file else None)
    a = audio_out(p)
    p.cable(m, 'Main left', a, '#0').cable(m, 'Main right', a, '#1')
    out['rack-machine'] = p

    # ch10 sample chain: Spoon Bender -> OTIS -> Vastish, touching, in that order (row 1);
    # Vastish plays from FM hits; its audio goes through Spoon Bender to the Audio module.
    p, g = base()
    sp = p.add('Fundamental', 'Split', gap=2)
    sg = p.add('Fundamental', 'Split')
    sb = p.add('SpoonBenderLab', 'SpoonBender', x=0, y=1)
    ot = p.add('OtisLab', 'OtisSampler', y=1)
    v = p.add('VastishLab', 'Vastish', y=1)
    a = p.add('Core', 'AudioInterface2', x=0, y=2, data=audio)
    p.cable(g, 'Pitch', sp, '#0').cable(g, 'Gates 1', sg, '#0')
    p.cable(sp, '#1', v, 'Pitch').cable(sg, '#1', v, 'Gate')
    p.cable(v, 'Left audio', sb, 'Left').cable(v, 'Right audio', sb, 'Right')
    p.cable(sb, 'Left', a, '#0').cable(sb, 'Right', a, '#1')
    out['rack-chain'] = p

    # ch11 Séance: OTIS with its included demo (embedded in DSP_Emulation's OTIS-start.vcv) -> Séance core
    # + Freeze + Scramble, touching
    start = _example(cfg, 'vcv/otislab/examples/OTIS-start.vcv')
    demo = next((m.get('data') for m in start['modules'] if m['model'] == 'OtisSampler'), None) if start else None
    p, g = base()
    ot = p.add('OtisLab', 'OtisSampler', x=0, y=1, params={'Loop': 1}, data=demo)
    se = p.add('SeanceLab', 'Seance', y=1, gap=2)
    p.add('SeanceLab', 'SeanceFreeze', y=1)
    p.add('SeanceLab', 'SeanceScramble', y=1)
    a = p.add('Core', 'AudioInterface2', x=0, y=2, data=audio)
    p.cable(g, 'Gate 1', ot, 'Play gate')
    p.cable(ot, 'Audio L', se, 'A')
    p.cable(se, 'Out', a, '#0').cable(se, 'Out', a, '#1')
    out['rack-seance'] = p

    # ch11 Bridge: Séance Bridge's feedback example (vcv/seance/examples/OTIS-spectral-feedback.vcv) as shipped,
    # module for module and cable for cable; only its audio device comes from the harvest.
    ex = _example(cfg, 'vcv/seance/examples/OTIS-spectral-feedback.vcv')
    if ex:
        p, h = Patch(cat), {}
        for m in ex['modules']:
            if m['plugin'] == 'Core':
                h[m['id']] = p.add('Core', 'AudioInterface2', x=m['pos'][0], y=m['pos'][1], data=audio)
            else:
                h[m['id']] = p.add(m['plugin'], m['model'], x=m['pos'][0], y=m['pos'][1],
                                   params={q['id']: q['value'] for q in m.get('params', [])}, data=m.get('data'))
        for c in ex['cables']:
            p.cable(h[c['outputModuleId']], f"#{c['outputId']}", h[c['inputModuleId']], f"#{c['inputId']}")
        out['rack-bridge'] = p

    # ch12 CV back: an LFO beside Gorgon, unpatched
    p, g = base()
    p.add('Fundamental', 'LFO', gap=2, params={'Frequency': -3})
    out['rack-cv-start'] = p

    # ch13 Studio's audio: OPT/BRK -> mixer -> Audio
    p, g = base(with_capture=True)
    m = p.add('Fundamental', 'VCMixer', gap=2)
    a = audio_out(p)
    p.cable(g, 'Optical audio left', m, '#1').cable(g, 'Breaks audio left', m, '#2')
    p.cable(m, '#0', a, '#0').cable(m, '#0', a, '#1')
    out['rack-audio'] = p

    # ch13 OFFSET: Gate 1 and Breaks L on a scope
    p, g = base(with_capture=True)
    s = p.add('Fundamental', 'Scope', gap=2)
    p.cable(g, 'Gate 1', s, '#0').cable(g, 'Breaks audio left', s, '#1')
    out['rack-offset'] = p

    # Harvest: configure by hand once (CLAUDE.md), save, and extract the data blocks.
    p, g = base()
    audio_out(p)
    out['harvest'] = p
    return out
