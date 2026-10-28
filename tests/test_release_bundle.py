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


def plugin(folder, slug, packed_version='2.0.0', extra=()):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'plugin.json').write_text(json.dumps(dict(slug=slug, version='2.0.0')))
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w') as archive:
        for name, data in [('plugin.json', json.dumps(dict(slug=slug, version=packed_version)).encode()),
                           ('plugin.so', b'fixture library'), *extra]:
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
        for name in set(release.DSP_PLUGINS) | release.RETIRED_DSP_PLUGINS:
            plugin(self.dsp / 'vcv' / name, name)
        for name in ('machinemodule', 'monomodule'):
            metadata = json.loads((self.dsp / 'vcv' / name / 'plugin.json').read_text())
            with tarfile.open(self.dsp / 'vcv' / name / 'dist' / f"{metadata['slug']}-2.0.0-source.tar.gz", 'w:gz') as package:
                info = tarfile.TarInfo('source/LICENSE')
                info.size = 7
                package.addfile(info, io.BytesIO(b'license'))
        self.dsp_sha = commit(self.dsp)
        members = ['hydrapy/studio.py', 'hydrapy/tracker/demo.json',
                   'hydrapy/tracker/app.js', 'hydrapy/tracker/editor.js',
                   'hydrapy/tracker/style.css', 'hydrapy/tracker/index.html',
                   'hydrapy/studio.html', 'hydrapy/tracker_v3.py']
        for name in members:
            p = self.hydrapy / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('fixture ' + name)
        (self.hydrapy / 'patches').mkdir()
        for name in ('gorgon_corridor.py', 'gorgon_scenes.py'):
            (self.hydrapy / 'patches' / name).write_text('# patch')
        for name in ('pyproject.toml', 'README.md', 'LICENSE'):
            (self.hydrapy / name).write_text('fixture ' + name)
        self.hydrapy_sha = commit(self.hydrapy)
        self.wheel = self.python_dist / 'hydrapy-0.1.0-py3-none-any.whl'
        with zipfile.ZipFile(self.wheel, 'w') as package:
            for name in members:
                package.writestr(name, (self.hydrapy / name).read_bytes())
        self.sdist = self.python_dist / 'hydrapy-0.1.0.tar.gz'
        self.write_sdist()
        self.manifest = dict(format='gorgon-preview-1', tag='preview-1', rack_sdk='2.6.4',
                             dsp_commit=self.dsp_sha, blocked_by=['Fix a known transport defect'])
        (self.root / 'release.json').write_text(json.dumps(self.manifest))
        (self.root / 'hydrapy.pin').write_text(self.hydrapy_sha + '\n')
        self.gorgon_sha = commit(self.root)

    def write_sdist(self, omit=None, stale=None):
        with tarfile.open(self.sdist, 'w:gz') as package:
            for name in release.git(self.hydrapy, 'ls-files').splitlines():
                if name == omit:
                    continue
                data = b'stale' if name == stale else (self.hydrapy / name).read_bytes()
                info = tarfile.TarInfo('hydrapy-0.1.0/' + name)
                info.size = len(data)
                package.addfile(info, io.BytesIO(data))

    def test_wheel_missing_python_module_or_studio_html_is_rejected(self):
        original = self.wheel.read_bytes()
        for missing in ('hydrapy/tracker_v3.py', 'hydrapy/studio.html'):
            with zipfile.ZipFile(io.BytesIO(original)) as source, zipfile.ZipFile(self.wheel, 'w') as target:
                for name in source.namelist():
                    if name != missing:
                        target.writestr(name, source.read(name))
            with self.assertRaisesRegex(ValueError, 'wheel package manifest differs'):
                self.assemble()

    def test_sdist_missing_or_stale_source_is_rejected(self):
        for name in ('hydrapy/tracker_v3.py', 'pyproject.toml'):
            self.write_sdist(omit=name)
            with self.assertRaisesRegex(ValueError, 'sdist is missing'):
                self.assemble()
            self.write_sdist(stale=name)
            with self.assertRaisesRegex(ValueError, 'sdist does not match'):
                self.assemble()

    def assemble(self):
        return release.assemble(self.root, self.dsp, self.hydrapy, self.python_dist, 'lin-x64', self.output)

    def test_bundle_has_all_nine_plugins_exact_pins_and_valid_checksums(self):
        target = self.assemble()
        original = target.read_bytes()
        self.assertEqual(original, self.assemble().read_bytes())
        prefix = 'Gorgon-preview-1-Linux-x64/'
        with zipfile.ZipFile(target) as archive:
            self.assertEqual(len([n for n in archive.namelist() if n.endswith('.vcvplugin')]), 9)
            self.assertFalse(any('drumlab' in n.lower() for n in archive.namelist()))
            manifest = json.loads(archive.read(prefix + 'RELEASE.json'))
            self.assertEqual(manifest['sources'], dict(Gorgon=self.gorgon_sha, hydrapy=self.hydrapy_sha,
                                                      DSP_Emulation=self.dsp_sha))
            checksums = archive.read(prefix + 'SHA256SUMS').decode().splitlines()
            payload = {n[len(prefix):] for n in archive.namelist()} - {'SHA256SUMS'}
            self.assertEqual({line.split('  ')[1] for line in checksums}, payload)
            for line in checksums:
                digest, name = line.split('  ')
                self.assertEqual(hashlib.sha256(archive.read(prefix + name)).hexdigest(), digest)

    def test_lookup_tables_require_exact_pinned_source_bytes(self):
        folder = self.dsp / 'vcv/bytebus'
        (folder / 'res').mkdir()
        (folder / 'res/lookup-v3.bin').write_bytes(b'generated table fixture')
        metadata = {'slug': 'ByteBusLab', 'version': '2.0.0'}
        package = folder / 'dist/ByteBusLab-2.0.0-lin-x64.vcvplugin'
        plugin(folder, 'ByteBusLab', extra=[('res/lookup-v3.bin', b'generated table fixture')])
        release.check_plugin(package, metadata, 'plugin.so', folder)
        plugin(folder, 'ByteBusLab', extra=[('res/lookup-v3.bin', b'foreign data')])
        with self.assertRaisesRegex(ValueError, 'firmware'):
            release.check_plugin(package, metadata, 'plugin.so', folder)

    def test_os_adapter_rejects_firmware_payload(self):
        folder = self.dsp / 'vcv' / 'machinemodule'
        plugin(folder, 'MachinemoduleRack', extra=[('docs/private.syx', b'synthetic test marker')])
        package = folder / 'dist/MachinemoduleRack-2.0.0-lin-x64.vcvplugin'
        with self.assertRaisesRegex(ValueError, 'forbidden OS-adapter payload'):
            release.check_plugin(package, {'slug': 'MachinemoduleRack', 'version': '2.0.0'}, 'plugin.so')

    def test_new_plugin_requires_explicit_workflow_update(self):
        plugin(self.dsp / 'vcv/newplugin', 'NewPlugin')
        with self.assertRaisesRegex(ValueError, 'DSP plugin set changed'):
            self.assemble()

    def test_source_archive_rejects_os_zip(self):
        source = self.dsp / 'vcv/machinemodule/dist/machinemodule-2.0.0-source.tar.gz'
        with tarfile.open(source, 'w:gz') as package:
            data = b'synthetic archive marker, no firmware'
            info = tarfile.TarInfo('source/private-os.ZIP')
            info.size = len(data)
            package.addfile(info, io.BytesIO(data))
        with self.assertRaisesRegex(ValueError, 'forbidden firmware payload'):
            self.assemble()

    def test_source_zip_exception_requires_exact_pinned_blob(self):
        folder = self.dsp / 'vcv/machinemodule'
        dependency = folder / '_deps/machinemodule/libs/dsp56300'
        relative = 'samples/help/doc.zip'
        fixture = dependency / 'source/wxWidgets' / relative
        fixture.parent.mkdir(parents=True)
        fixture.write_bytes(b'synthetic source fixture')
        revision = commit(dependency)
        (folder / 'docs').mkdir()
        (folder / 'docs/SOURCE_REVISIONS.txt').write_text('libs/dsp56300: ' + revision + '\n')
        source = folder / 'dist/source-test.tar.gz'
        for data in (fixture.read_bytes(), b'foreign archive'):
            with tarfile.open(source, 'w:gz') as package:
                info = tarfile.TarInfo('MachinemoduleRack/vcv/machinemodule/_deps/machinemodule/libs/dsp56300/source/wxWidgets/' + relative)
                info.size = len(data)
                package.addfile(info, io.BytesIO(data))
            if data == fixture.read_bytes():
                release.check_source_archive(source, folder, 'MachinemoduleRack')
            else:
                with self.assertRaisesRegex(ValueError, 'forbidden firmware payload'):
                    release.check_source_archive(source, folder, 'MachinemoduleRack')

    def test_plugin_rejects_archived_and_raw_os_extensions(self):
        folder = self.dsp / 'vcv/bytebus'
        for suffix in ('.zip', '.img', '.hex'):
            with self.subTest(suffix=suffix):
                plugin(folder, 'ByteBusLab', extra=[('res/private-os' + suffix, b'synthetic marker')])
                package = folder / 'dist/ByteBusLab-2.0.0-lin-x64.vcvplugin'
                with self.assertRaisesRegex(ValueError, 'firmware'):
                    release.check_plugin(package, {'slug': 'ByteBusLab', 'version': '2.0.0'}, 'plugin.so', folder)

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
