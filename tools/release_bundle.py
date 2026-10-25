#!/usr/bin/env python3
"""Assemble one pinned Gorgon preview from already-built packages.

This command does not build plugins, configure audio devices or publish releases.
Run with --publish-check before creating a release; blockers allow dry-run builds.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ARCHES = {
    'lin-x64': ('Linux-x64', 'plugin.so'),
    'win-x64': ('Windows-x64', 'plugin.dll'),
    'mac-arm64': ('macOS-AppleSilicon', 'plugin.dylib'),
    'mac-x64': ('macOS-Intel', 'plugin.dylib'),
}
DSP_PLUGINS = ('otislab', 'vastish', 'spoonbender', 'seance', 'bytebus')


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()


def config(root=ROOT, *, publish=False):
    release = json.loads((root / 'release.json').read_text())
    if release.get('format') != 'gorgon-preview-1':
        raise ValueError('Expected a gorgon-preview-1 release manifest')
    if not re.fullmatch(r'preview-[A-Za-z0-9][A-Za-z0-9.-]*', release.get('tag', '')):
        raise ValueError('Use a preview-* release tag without path separators')
    if not re.fullmatch(r'\d+\.\d+\.\d+', release.get('rack_sdk', '')):
        raise ValueError('Pin the Rack SDK version')
    for name, sha in [('dsp_commit', release.get('dsp_commit', '')),
                      ('hydrapy.pin', (root / 'hydrapy.pin').read_text().strip())]:
        if not re.fullmatch(r'[0-9a-f]{40}', sha):
            raise ValueError(f'{name} must pin a full commit SHA')
    blockers = release.get('blocked_by')
    if not isinstance(blockers, list) or any(not isinstance(x, str) or not x.strip() for x in blockers):
        raise ValueError('blocked_by must be a list of release blockers')
    if publish and blockers:
        raise ValueError('Release is blocked:\n' + '\n'.join('- ' + x for x in blockers))
    return release


def check_checkout(repo, expected):
    actual = git(repo, 'rev-parse', 'HEAD')
    if actual != expected:
        raise ValueError(f'{repo}: expected {expected}, found {actual}')
    if git(repo, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError(f'{repo}: tracked source changes are not committed')
    return actual


def check_plugin(path, metadata, library):
    payload = subprocess.check_output(['zstd', '-q', '-d', '-c', str(path)])
    with tarfile.open(fileobj=io.BytesIO(payload)) as archive:
        prefix = metadata['slug'] + '/'
        member = archive.extractfile(prefix + 'plugin.json')
        if member is None:
            raise ValueError(f'{path}: missing plugin.json')
        packed = json.load(member)
        if any(packed.get(k) != metadata[k] for k in ('slug', 'version')):
            raise ValueError(f'{path}: package metadata does not match its source')
        if not archive.getmember(prefix + library).isfile():
            raise ValueError(f'{path}: missing {library}')


def quickstart(tag, arch, wheel, sources):
    return f'''Gorgon {tag} ({ARCHES[arch][0]})

Rack plugins
1. In VCV Rack 2, choose Help > Open user folder, then quit Rack.
2. Copy the six unopened rack/*.vcvplugin files into plugins-{arch} there.
3. Restart Rack. Search for Gorgon, OTIS Sampler, Vastish, Spoon Bender,
   Séance, Converter, Clocked Register and Lookup.

Tracker and visuals (Python 3.9+ must be installed separately)
Open a terminal in this extracted folder. Use python3 on Linux/macOS or
py on Windows, and create a virtual environment:

    python3 -m venv .venv

Activate it with `source .venv/bin/activate` on Linux/macOS, or with
`.venv\\Scripts\\Activate.ps1` in Windows PowerShell. Then run:

    python -m pip install "./python/{wheel}[gpu,video,jack,cv]"
    hydrapy studio patches/gorgon_corridor.py --tracker

The pip command needs internet access to install dependencies. JACK/PipeWire
must be installed and running separately if you enable its audio ports.
Open the local URL Studio prints. Space plays/pauses. Sound enables browser
audition; CV and Rack are separate outputs. Program/Auxiliary open monitors.
Save session keeps the score. No interface or hardware route is auto-configured.

Open examples/gorgon-bytebus.vcv in Rack for the Gorgon BUS -> Register -> DAC
example. Select Gorgon's BUS source in its context menu. For pitched voices,
patch Gorgon's pitch and gates into OTIS or Vastish separately.

For named visual scenes, start patches/gorgon_scenes.py instead.
The native desktop tracker/core extraction is future work; this preview uses
the browser tracker and Python renderer. Browser synth audio is not exported.

Exact source commits and plugin versions: RELEASE.json.
Verify the extracted files against SHA256SUMS before installing.
Documentation: https://github.com/JosiahAlanLee/Gorgon/blob/{sources['Gorgon']}/README.md
Audio routing: https://github.com/JosiahAlanLee/hydrapy/blob/{sources['hydrapy']}/docs/gorgon-audio.md
ES-40 setup: https://github.com/JosiahAlanLee/hydrapy/blob/{sources['hydrapy']}/docs/es40.md
Instruments: https://github.com/JosiahAlanLee/DSP_Emulation/blob/{sources['DSP_Emulation']}/README.md

Linux software paths have been exercised. Windows/macOS plugins are built by
CI; those builds have not been played inside Rack. Physical interfaces,
ES-40 transport and end-to-end audio/display latency require workstation tests.
'''


def assemble(root, dsp, hydrapy, python_dist, arch, output):
    release = config(root)
    hydrapy_sha = (root / 'hydrapy.pin').read_text().strip()
    sources = {
        'Gorgon': check_checkout(root, git(root, 'rev-parse', 'HEAD')),
        'hydrapy': check_checkout(hydrapy, hydrapy_sha),
        'DSP_Emulation': check_checkout(dsp, release['dsp_commit']),
    }
    files, plugins = {}, []
    for folder in [root / 'rack', *(dsp / 'vcv' / name for name in DSP_PLUGINS)]:
        metadata = json.loads((folder / 'plugin.json').read_text())
        if not metadata['version'].startswith('2.'):
            raise ValueError(f'{folder}: Rack 2 requires a 2.* plugin version')
        name = f"{metadata['slug']}-{metadata['version']}-{arch}.vcvplugin"
        package = folder / 'dist' / name
        check_plugin(package, metadata, ARCHES[arch][1])
        files['rack/' + name] = package.read_bytes()
        plugins.append({k: metadata[k] for k in ('slug', 'version')})
    wheels = sorted(python_dist.glob('hydrapy-*-py3-none-any.whl'))
    if len(wheels) != 1:
        raise ValueError('Provide exactly one pinned HydraPy wheel')
    wheel = wheels[0]
    with zipfile.ZipFile(wheel) as package:
        required = {'hydrapy/studio.py', 'hydrapy/tracker/demo.json',
                    'hydrapy/tracker/app.js', 'hydrapy/tracker/editor.js',
                    'hydrapy/tracker/style.css', 'hydrapy/tracker/index.html'}
        if not required.issubset(package.namelist()):
            raise ValueError('HydraPy wheel is missing the tracker or Studio assets')
        for member in package.namelist():
            if member.startswith('hydrapy/') and not member.endswith('/'):
                source = hydrapy / member
                if not source.is_file() or package.read(member) != source.read_bytes():
                    raise ValueError(f'HydraPy wheel does not match the pinned source: {member}')
    files['python/' + wheel.name] = wheel.read_bytes()
    sdists = sorted(python_dist.glob('hydrapy-*.tar.gz'))
    if len(sdists) != 1:
        raise ValueError('Provide the matching HydraPy source distribution')
    files['python/' + sdists[0].name] = sdists[0].read_bytes()
    for name in ('gorgon_corridor.py', 'gorgon_scenes.py'):
        files['patches/' + name] = (hydrapy / 'patches' / name).read_bytes()
    files['examples/gorgon-bytebus.vcv'] = (root / 'rack/examples/gorgon-bytebus.vcv').read_bytes()
    files['INSTALL.txt'] = quickstart(release['tag'], arch, wheel.name, sources).encode()
    files['RELEASE.json'] = (json.dumps(dict(release, architecture=arch, sources=sources,
                                           plugins=plugins, hydrapy_wheel=wheel.name), indent=2) + '\n').encode()
    checksums = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                        for name, data in sorted(files.items()))
    files['SHA256SUMS'] = checksums.encode()
    name = f"Gorgon-{release['tag']}-{ARCHES[arch][0]}"
    output.mkdir(parents=True, exist_ok=True)
    target = output / (name + '.zip')
    temporary = target.with_suffix('.tmp')
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path, data in sorted(files.items()):
                info = zipfile.ZipInfo(name + '/' + path, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, data)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--publish-check', action='store_true')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--dsp', type=Path)
    parser.add_argument('--hydrapy', type=Path)
    parser.add_argument('--python-dist', type=Path)
    parser.add_argument('--arch', choices=ARCHES)
    parser.add_argument('--output', type=Path, default=ROOT / 'release-dist')
    args = parser.parse_args()
    release = config(args.root, publish=args.publish_check)
    if args.check or args.publish_check:
        print(json.dumps(release, indent=2))
        return
    if any(v is None for v in (args.dsp, args.hydrapy, args.python_dist, args.arch)):
        parser.error('assembly needs --dsp, --hydrapy, --python-dist and --arch')
    print(assemble(args.root, args.dsp, args.hydrapy, args.python_dist, args.arch, args.output))


if __name__ == '__main__':
    main()
