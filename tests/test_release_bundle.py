"""Release integrity: pins, packaged metadata, wheel contents and checksums."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location('release_bundle', Path(__file__).resolve().parents[1] / 'tools/release_bundle.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


def commit(path):
    subprocess.run(['git', 'init', '-q', str(path)], check=True)
    subprocess.run(['git', '-C', str(path), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(path), '-c', 'user.name=Release test',
                    '-c', 'user.email=release@example.invalid', 'commit', '-qm', 'Fixture'], check=True)
    return release.git(path, 'rev-parse', 'HEAD')


def plugin(folder, slug, packed_version='2.0.0'):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'plugin.json').write_text(json.dumps(dict(slug=slug, version='2.0.0')))
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w') as archive:
        for name, data in [('plugin.json', json.dumps(dict(slug=slug, version=packed_version)).encode()),
                           ('plugin.so', b'fixture library')]:
            info = tarfile.TarInfo(slug + '/' + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    output = folder / 'dist' / f'{slug}-2.0.0-lin-x64.vcvplugin'
    output.parent.mkdir(exist_ok=True)
    output.write_bytes(subprocess.check_output(['zstd', '-q', '-c'], input=payload.getvalue()))


@unittest.skipUnless(shutil.which('zstd'), 'zstd is required to inspect real Rack packages')
class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root, self.dsp, self.hydrapy = (base / x for x in ('Gorgon', 'DSP', 'hydrapy'))
        self.python_dist, self.output = base / 'python-dist', base / 'out'
        for p in (self.root, self.dsp, self.hydrapy, self.python_dist):
            p.mkdir()
        for p in (self.root, self.dsp):
            (p / '.gitignore').write_text('dist/\n')
        plugin(self.root / 'rack', 'GorgonBridge')
        (self.root / 'rack/examples').mkdir()
        (self.root / 'rack/examples/gorgon-bytebus.vcv').write_text('{}')
        for name in release.DSP_PLUGINS:
            plugin(self.dsp / 'vcv' / name, name)
        self.dsp_sha = commit(self.dsp)
        members = ['hydrapy/studio.py', 'hydrapy/tracker/demo.json',
                   'hydrapy/tracker/app.js', 'hydrapy/tracker/editor.js',
                   'hydrapy/tracker/style.css', 'hydrapy/tracker/index.html']
        for name in members:
            p = self.hydrapy / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('fixture ' + name)
        (self.hydrapy / 'patches').mkdir()
        for name in ('gorgon_corridor.py', 'gorgon_scenes.py'):
            (self.hydrapy / 'patches' / name).write_text('# patch')
        self.hydrapy_sha = commit(self.hydrapy)
        self.wheel = self.python_dist / 'hydrapy-0.1.0-py3-none-any.whl'
        with zipfile.ZipFile(self.wheel, 'w') as package:
            for name in members:
                package.writestr(name, (self.hydrapy / name).read_bytes())
        (self.python_dist / 'hydrapy-0.1.0.tar.gz').write_bytes(b'source fixture')
        self.manifest = dict(format='gorgon-preview-1', tag='preview-1', rack_sdk='2.6.4',
                             dsp_commit=self.dsp_sha, blocked_by=['Fix a known transport defect'])
        (self.root / 'release.json').write_text(json.dumps(self.manifest))
        (self.root / 'hydrapy.pin').write_text(self.hydrapy_sha + '\n')
        self.gorgon_sha = commit(self.root)

    def assemble(self):
        return release.assemble(self.root, self.dsp, self.hydrapy, self.python_dist, 'lin-x64', self.output)

    def test_bundle_has_all_six_plugins_exact_pins_and_valid_checksums(self):
        target = self.assemble()
        original = target.read_bytes()
        self.assertEqual(original, self.assemble().read_bytes())
        prefix = 'Gorgon-preview-1-Linux-x64/'
        with zipfile.ZipFile(target) as archive:
            self.assertEqual(len([n for n in archive.namelist() if n.endswith('.vcvplugin')]), 6)
            manifest = json.loads(archive.read(prefix + 'RELEASE.json'))
            self.assertEqual(manifest['sources'], dict(Gorgon=self.gorgon_sha, hydrapy=self.hydrapy_sha,
                                                      DSP_Emulation=self.dsp_sha))
            checksums = archive.read(prefix + 'SHA256SUMS').decode().splitlines()
            payload = {n[len(prefix):] for n in archive.namelist()} - {'SHA256SUMS'}
            self.assertEqual({line.split('  ')[1] for line in checksums}, payload)
            for line in checksums:
                digest, name = line.split('  ')
                self.assertEqual(hashlib.sha256(archive.read(prefix + name)).hexdigest(), digest)

    def test_publication_refuses_blockers_while_dry_run_remains_available(self):
        release.config(self.root)
        with self.assertRaisesRegex(ValueError, 'known transport defect'):
            release.config(self.root, publish=True)

    def test_mismatched_or_dirty_source_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'expected .* found'):
            release.check_checkout(self.dsp, 'a' * 40)
        (self.hydrapy / 'hydrapy/studio.py').write_text('uncommitted edit')
        with self.assertRaisesRegex(ValueError, 'not committed'):
            self.assemble()

    def test_stale_renamed_plugin_and_stale_wheel_are_rejected(self):
        plugin(self.dsp / 'vcv' / 'bytebus', 'bytebus', packed_version='2.9.9')
        with self.assertRaisesRegex(ValueError, 'metadata does not match'):
            self.assemble()
        plugin(self.dsp / 'vcv' / 'bytebus', 'bytebus')
        # Rebuild a wheel from different source without changing its filename.
        with zipfile.ZipFile(self.wheel, 'w') as package:
            for p in sorted((self.hydrapy / 'hydrapy').rglob('*')):
                if p.is_file():
                    name = p.relative_to(self.hydrapy).as_posix()
                    package.writestr(name, b'outdated code' if name.endswith('studio.py') else p.read_bytes())
        with self.assertRaisesRegex(ValueError, 'wheel does not match'):
            self.assemble()


if __name__ == '__main__':
    unittest.main()
