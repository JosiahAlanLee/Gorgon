"""The action vocabulary for shot scripts. Each takes (stage, argument).

A string argument is the action's main parameter; a mapping gives keywords.
`gtut actions` prints this table.
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path

ACTIONS = {}


def action(name, doc):
    def wrap(fn):
        fn.doc = doc
        ACTIONS[name] = fn
        return fn
    return wrap


def _kw(arg, main):
    return arg if isinstance(arg, dict) else {main: arg}


# timing ------------------------------------------------------------------
@action('wait', 'seconds: hold still')
def _wait(st, arg):
    time.sleep(float(arg))


# Studio controller -------------------------------------------------------
@action('studio.click', 'target: CSS selector, or {role,name} / {label} / {text}; the cursor moves there and clicks')
def _s_click(st, arg):
    st.controller.click(arg)


@action('studio.point', 'target: move the cursor onto an element without clicking')
def _s_point(st, arg):
    st.controller.point(arg)


@action('studio.fill', '{target, text, press=Tab}: click a field, select all, type')
def _s_fill(st, arg):
    st.controller.fill(arg['target'], arg['text'], press=arg.get('press', 'Tab'))


@action('studio.select', '{target, value}: open a dropdown and choose a value')
def _s_select(st, arg):
    st.controller.select(arg['target'], arg['value'])


@action('studio.press', 'key: a Playwright key name (Space, Enter, Control+Z, Alt+ArrowUp)')
def _s_press(st, arg):
    for key in (arg if isinstance(arg, list) else [arg]):
        st.controller.press(key)
        time.sleep(0.15)


@action('studio.type', 'text: type into the focused element at a readable rate')
def _s_type(st, arg):
    kw = _kw(arg, 'text')
    st.controller.type(kw['text'], cps=kw.get('cps', 8))


@action('studio.wait', '{js} or {selector}: wait for the page')
def _s_wait(st, arg):
    kw = _kw(arg, 'js')
    st.controller.wait_for(js=kw.get('js'), selector=kw.get('selector'), timeout=kw.get('timeout', 15))


@action('studio.eval', 'js: evaluate in the controller (setup only; nothing visible should depend on it)')
def _s_eval(st, arg):
    st.controller.eval(arg)


@action('studio.api', '{action, ...}: call Studio directly, off camera (load, tracker_transport, jack_audio ...)')
def _s_api(st, arg):
    body = dict(arg)
    st.api.call(body.pop('action'), **body)


@action('studio.open', 'program | aux: open an output window and place it on the stage')
def _s_open(st, arg):
    st.controller.open_output(arg or 'program')


@action('studio.reload', 'reload the controller (after an off-camera score change)')
def _s_reload(st, arg):
    st.controller.page.reload()
    st.controller.page.wait_for_function('window.gorgonPreview?.frames >= 1', timeout=30_000)
    st.controller.offset = st.controller.calibrate()


# Rack ----------------------------------------------------------------------
@action('rack.load', 'patch: a built patch name (work/patches/<name>.vcv) or a path')
def _r_load(st, arg):
    st.rack.load(st.patch_path(arg))


@action('rack.view', '{modules, zoom, seconds}: animate the rack view to fit modules')
def _r_view(st, arg):
    kw = _kw(arg, 'modules')
    st.rack.view(kw.get('modules'), kw.get('zoom'), kw.get('seconds', 1.2), kw.get('margin', 24))


@action('rack.highlight', '{ref, label, seconds}: ring a module, port or param, with a caption')
def _r_highlight(st, arg):
    kw = _kw(arg, 'ref')
    st.rack.highlight(kw['ref'], kw.get('label', ''), kw.get('seconds', 3))


@action('rack.point', 'ref: move the cursor onto a module, port or param')
def _r_point(st, arg):
    st.rack.point(arg)


@action('rack.click', 'ref: click a button or port')
def _r_click(st, arg):
    st.rack.click(arg)


@action('rack.cable', '{from, to}: drag a cable from an output to an input, then check it connected')
def _r_cable(st, arg):
    st.rack.cable(arg['from'], arg['to'], arg.get('seconds'))


@action('rack.unplug', 'ref: pull the cable off an input')
def _r_unplug(st, arg):
    st.rack.unplug(arg)


@action('rack.knob', '{ref, value | scaled, seconds}: hover a control and turn it smoothly')
def _r_knob(st, arg):
    st.rack.knob(arg['ref'], arg.get('value'), arg.get('scaled'), arg.get('seconds', 1.5))


@action('rack.menu', '{ref, path: [item, sub-item]}: right-click and walk a context menu by text')
def _r_menu(st, arg):
    st.rack.menu(arg['ref'], arg['path'], arg.get('pause', 0.5))


@action('rack.add', '{query, fallback}: add a module from the browser; fallback is a patch to cut to')
def _r_add(st, arg):
    kw = _kw(arg, 'query')
    fb = st.patch_path(kw['fallback']) if kw.get('fallback') else None
    st.rack.add_module(kw['query'], fallback_patch=fb)


@action('rack.escape', 'close an open menu or browser')
def _r_escape(st, arg):
    st.rack.escape()


@action('rack.save', 'path: save the running patch (to harvest GUI-only module state)')
def _r_save(st, arg):
    st.rack.save(Path(arg))


# Terminal -----------------------------------------------------------------
@action('term.run', '{cmd, wait_for, timeout}: type a command, press Enter, optionally wait for output (regex)')
def _t_run(st, arg):
    kw = _kw(arg, 'cmd')
    st.term.run(kw['cmd'], kw.get('wait_for'), kw.get('timeout', 600), kw.get('cps', 16))


@action('term.type', '{cmd, cps}: type a command and press Enter, without waiting (pair with term.wait)')
def _t_type(st, arg):
    kw = _kw(arg, 'cmd')
    st.term.type(kw['cmd'], kw.get('cps', 16))


@action('term.wait', '{wait_for, timeout}: wait for the last typed command\'s output; a speed range can start here,'
                     ' so a long command is typed at normal speed and only its run is fast-forwarded')
def _t_wait(st, arg):
    kw = _kw(arg, 'wait_for')
    st.term.wait(kw['wait_for'], kw.get('timeout', 600))


@action('term.quiet', 'cmd: run without typing it on camera, then clear')
def _t_quiet(st, arg):
    st.term.quiet(arg)


@action('term.clear', 'clear the terminal')
def _t_clear(st, arg):
    st.term.clear()


@action('term.interrupt', 'Ctrl+C')
def _t_int(st, arg):
    st.term.interrupt()


# Hand and stage -----------------------------------------------------------
@action('hand.park', 'window: move the cursor to a corner of a stage window')
def _h_park(st, arg):
    st.hand.park(st.cfg['stage']['windows'][arg])


@action('hand.move', '{window, x, y}: move to a point given as fractions (0..1) of a stage window')
def _h_move(st, arg):
    x, y, w, h = st.cfg['stage']['windows'][arg['window']]
    st.hand.move(x + w * arg['x'], y + h * arg['y'], seconds=arg.get('seconds'))


@action('place', '{name, window, timeout}: move the X window titled `name` (Studio\'s native Program window)'
                 ' onto a stage window')
def _place(st, arg):
    from .display import place, wait_window
    wid = wait_window(st.cfg, timeout=arg.get('timeout', 10), name=arg['name'])
    place(st.cfg, wid, st.cfg['stage']['windows'][arg['window']])


@action('shell', 'cmd: run a command off camera, with the stage environment, in the terminal\'s sandbox')
def _shell(st, arg):
    # The same folder the on-camera terminal starts in (work/sandbox), so a path in a script means one thing.
    # Not work/ itself: that holds the stage's own HydraPy worktree, venv and takes, and setups like
    # `rm -rf hydrapy` (clearing the on-camera clone) would delete the HydraPy that Studio runs from.
    sandbox = st.cfg.work / 'sandbox'
    sandbox.mkdir(parents=True, exist_ok=True)
    subprocess.run(arg, shell=True, check=True, env=st.env(), cwd=sandbox)


@action('screenshot', 'name: save a stage screenshot into the take folder (for QC)')
def _shot(st, arg):
    from .display import screenshot
    screenshot(st.cfg, st.take_dir_current / f'{arg}.png')
