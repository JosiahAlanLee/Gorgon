"""Read JACK's actual graph under the same wrapper used to start the apps."""
from __future__ import annotations

import ctypes as C
import json
import os
import re
import shutil
import subprocess
import sys

from .config import LaunchError


def environment(config):
    env = dict(os.environ, JACK_NO_START_SERVER='1')
    if config['audio']['backend'] == 'pipewire':
        env.update(PIPEWIRE_LATENCY=f"{config['audio']['quantum']}/{config['audio']['rate']}",
                   PIPEWIRE_RATE=f"1/{config['audio']['rate']}")
    return env


def rack_environment(config, env):
    # Rack 2 finds a device's RtAudio ID in one RtAudio instance and opens it in another; each numbers devices
    # in JACK port order, which PipeWire sorts by priority and then by reusable node ID. Rack's first audio
    # client could take the number of the device the next module opens, unless Rack's clients sort last.
    if config['audio']['backend'] != 'pipewire':
        return env
    return dict(env, PIPEWIRE_PROPS=env.get('PIPEWIRE_PROPS') or '{ priority.session = -1 }')


def wrap(config, command):
    return (['pw-jack'] if config['audio']['backend'] == 'pipewire' else []) + list(command)


def probe(config, operations=()):
    env = environment(config)
    # Discovery must not request a different graph rate or quantum.
    env.pop('PIPEWIRE_LATENCY', None)
    env.pop('PIPEWIRE_RATE', None)
    command = wrap(config, [sys.executable, '-m', 'gorgon.audio', json.dumps(list(operations))])
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=8)
        data = json.loads(result.stdout.splitlines()[-1])
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired) as exc:
        raise LaunchError(f'Cannot query JACK: {exc}') from exc
    if result.returncode or data.get('error'):
        raise LaunchError(f"JACK is unavailable: {data.get('error') or result.stderr.strip()[-500:]}")
    return data


def ensure_server(config):
    if config['audio']['backend'] == 'pipewire':
        for tool in ('pw-jack', 'pw-cli'):
            if not shutil.which(tool):
                raise LaunchError(f'{tool} is missing; install your distribution\'s PipeWire JACK/client tools (see docs/LAUNCHER.md)')
        result = subprocess.run(['pw-cli', 'info', '0'], capture_output=True, timeout=8)
        if result.returncode:
            if not config['audio']['start_services']:
                raise LaunchError('PipeWire is not running; automatic user-service startup is disabled')
            if not shutil.which('systemctl'):
                raise LaunchError('PipeWire is not running and systemctl is unavailable; start your audio session first')
            result = subprocess.run(['systemctl', '--user', 'start', 'pipewire.socket', 'wireplumber.service'],
                                    capture_output=True, text=True, timeout=20)
            if result.returncode:
                raise LaunchError('Could not start PipeWire/WirePlumber user services: ' + result.stderr.strip()[-500:])
    return probe(config)


def playback_device(config, graph):
    inputs = [p for p in graph['ports'] if p['flags'] & 1]
    groups = {p['name'].split(':', 1)[0] for p in inputs}
    wanted = config['audio']['device']
    if wanted:
        if wanted not in groups:
            raise LaunchError(f'JACK playback device {wanted!r} is absent; input groups: {", ".join(sorted(groups)) or "none"}')
        return wanted
    physical = {p['name'].split(':', 1)[0] for p in inputs if p['flags'] & 4}
    # Resolve WirePlumber's default sink through JACK names and aliases.
    if config['audio']['backend'] == 'pipewire' and shutil.which('pw-dump'):
        names = default_sink_names()
        matches = {p['name'].split(':', 1)[0] for p in inputs
                   if any(n == alias.split(':', 1)[0] for alias in [p['name'], *p['aliases']] for n in names)}
        if len(matches) == 1:
            return next(iter(matches))
    if len(physical) == 1:
        return next(iter(physical))
    choices = ', '.join(sorted(physical or groups)) or 'none'
    raise LaunchError(f'Choose audio.device once in {config["_path"]}; available JACK input groups: {choices}')


def default_sink_names():
    # From pw-dump rather than `wpctl inspect @DEFAULT_AUDIO_SINK@`, which WirePlumber 0.4.8 rejects.
    try:
        objects = json.loads(subprocess.run(['pw-dump'], capture_output=True, text=True, timeout=5).stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return []
    default = next((entry['value'].get('name') for o in objects if (o.get('props') or {}).get('metadata.name') == 'default'
                    for entry in o.get('metadata') or []
                    if entry.get('key') == 'default.audio.sink' and isinstance(entry.get('value'), dict)), None)
    for o in objects:
        props = (o.get('info') or {}).get('props') or {}
        if default and props.get('node.name') == default:
            return [props[k] for k in ('node.name', 'node.nick', 'node.description') if props.get(k)]
    return []


def session_clock(config, client, graph):
    # Under PipeWire a client runs at its driver group's quantum, which the query client (in no group) does
    # not see; pw-top's profiler reports it per node. Native JACK has one buffer size for everyone.
    if config['audio']['backend'] == 'pipewire' and shutil.which('pw-top'):
        try:
            text = subprocess.run(['pw-top', '-b', '-n', '2'], capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired):
            text = ''
        for line in text.rsplit('QUANT', 1)[-1].splitlines():
            fields = line.split()
            if len(fields) > 9 and fields[0] == 'R' and fields[2].isdigit() and int(fields[2]) > 0 \
                    and fields[3].isdigit() and line.rstrip().endswith(' ' + client):
                return dict(rate=int(fields[3]), quantum=int(fields[2]))
    return dict(rate=graph['rate'], quantum=graph['quantum'])


def rack_ports(graph):
    return [p for p in graph['ports'] if re.search(r'rack|rtapi', p['name'].split(':', 1)[0], re.I)]


def _graph(operations):
    """Run only in a short-lived helper so libjack is loaded after pw-jack."""
    lib = C.CDLL('libjack.so.0')
    declarations = {
        'jack_client_open': (C.c_void_p, [C.c_char_p, C.c_uint, C.POINTER(C.c_uint)]),
        'jack_client_close': (C.c_int, [C.c_void_p]),
        'jack_get_sample_rate': (C.c_uint, [C.c_void_p]),
        'jack_get_buffer_size': (C.c_uint, [C.c_void_p]),
        'jack_get_ports': (C.POINTER(C.c_char_p), [C.c_void_p, C.c_char_p, C.c_char_p, C.c_ulong]),
        'jack_port_by_name': (C.c_void_p, [C.c_void_p, C.c_char_p]),
        'jack_port_flags': (C.c_int, [C.c_void_p]),
        'jack_port_get_all_connections': (C.POINTER(C.c_char_p), [C.c_void_p, C.c_void_p]),
        'jack_port_get_aliases': (C.c_int, [C.c_void_p, C.POINTER(C.c_char_p)]),
        'jack_port_name_size': (C.c_int, []),
        'jack_connect': (C.c_int, [C.c_void_p, C.c_char_p, C.c_char_p]),
        'jack_disconnect': (C.c_int, [C.c_void_p, C.c_char_p, C.c_char_p]),
        'jack_free': (None, [C.c_void_p]),
    }
    for name, (restype, argtypes) in declarations.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = restype, argtypes
    status = C.c_uint()
    client = lib.jack_client_open(f'gorgon-query-{os.getpid()}'.encode(), 1, C.byref(status))
    if not client:
        raise LaunchError(f'cannot connect to the existing JACK server (status {status.value})')
    def strings(pointer):
        values = []
        if pointer:
            try:
                i = 0
                while pointer[i]:
                    values.append(pointer[i].decode())
                    i += 1
            finally:
                lib.jack_free(C.cast(pointer, C.c_void_p))
        return values
    try:
        for operation, source, destination in operations:
            code = getattr(lib, 'jack_' + operation)(client, source.encode(), destination.encode())
            if code and not (operation == 'connect' and code == 17):
                raise LaunchError(f'JACK {operation} {source} → {destination} failed ({code})')
        ports = []
        for name in strings(lib.jack_get_ports(client, None, b'32 bit float mono audio', 0)):
            port = lib.jack_port_by_name(client, name.encode())
            if not port:
                continue
            buffers = [C.create_string_buffer(lib.jack_port_name_size()) for _ in range(2)]
            aliases = (C.c_char_p * 2)(*[C.cast(b, C.c_char_p) for b in buffers])
            count = lib.jack_port_get_aliases(port, aliases)
            ports.append(dict(name=name, flags=lib.jack_port_flags(port),
                              aliases=[b.value.decode() for b in buffers[:max(0, count)]],
                              connections=strings(lib.jack_port_get_all_connections(client, port))))
        return dict(rate=lib.jack_get_sample_rate(client), quantum=lib.jack_get_buffer_size(client), ports=ports)
    finally:
        lib.jack_client_close(client)


if __name__ == '__main__':
    try:
        print(json.dumps(_graph(json.loads(sys.argv[1]) if len(sys.argv) > 1 else [])))
    except Exception as exc:
        print(json.dumps(dict(error=str(exc))))
        sys.exit(1)
