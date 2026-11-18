# SPDX-License-Identifier: GPL-3.0-or-later
"""Encoding and dependency regressions; no device or build actions."""
import builtins
import contextlib
import hashlib
import io
import json
import ntpath
import os
from pathlib import Path, PureWindowsPath
import re
import runpy
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

from diskos_installer import __main__ as cli, imagebuild, service, ui
from diskos_installer.reporter import CLIReporter, Reporter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import verify_git_blobs


def windows_normalize_toc(toc):
    # Model datastruct.py:309-384 in PyInstaller 6.16.0, without importing it.
    # Equal-priority duplicates keep the first source; Windows keys ignore case.
    priorities = {'DEPENDENCY': 3, 'SYMLINK': 2, 'BINARY': 1, 'EXTENSION': 1}
    options, entries = [], {}
    for dest, source, kind in toc:
        if kind == 'OPTION':
            options.append((dest, source, kind))
            continue
        dest = ntpath.normpath(dest)
        key = PureWindowsPath(dest)
        previous = entries.get(key)
        if previous is None or priorities.get(kind, 0) > priorities.get(previous[2], 0):
            entries[key] = (dest, source, kind)
    return options + list(entries.values())


class ConfirmationTextTests(unittest.TestCase):
    def test_cli_prints_the_engine_consequence_before_confirming(self):
        for action, consequence in (("install", service.INSTALL_CONSEQUENCE),
                                    ("restore-stock", service.RESTORE_CONSEQUENCE)):
            for yes in (False, True):
                with self.subTest(action=action, yes=yes), mock.patch.object(ui, "step"), \
                        mock.patch.object(ui, "info") as info, \
                        mock.patch.object(ui, "confirm", return_value=False) as prompt:
                    def answer(*args, **kwargs):
                        info.assert_any_call("  consequence: " + consequence)
                        return False
                    prompt.side_effect = answer
                    result = cli._cli_confirm(yes)({"action": action, "consequence": consequence})
                    info.assert_any_call("  consequence: " + consequence)
                    self.assertEqual(result, yes)
                    if yes:
                        prompt.assert_not_called()
                    else:
                        prompt.assert_called_once_with("Proceed?", default=False)

    def test_destructive_cli_phase_keeps_the_cable_instruction(self):
        with mock.patch.object(ui, "step") as step:
            reporter = CLIReporter()
            reporter.phase("Flashing", destructive=True)
            step.assert_called_once_with("Flashing  (keep the USB cable connected)")
            step.reset_mock()
            reporter.phase("Building")
            step.assert_called_once_with("Building")


class WindowsBuildScriptTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1]
        self.script = (root / 'build/build-windows.ps1').read_text(encoding='ascii')

    def test_capability_checks_are_unchanged(self):
        # Keep the committed enablement gates verbatim, including argument forwarding.
        gate = '''    $capabilityArgs = @()
    if ($EnableWindowsFlash) {
        $revision = & git rev-parse --verify HEAD
        if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') {
            throw 'Windows flash capability requires a full installer commit SHA'
        }
        $dirty = & git status --porcelain --untracked-files=all
        if ($LASTEXITCODE -ne 0 -or $dirty) {
            throw 'Windows flash capability requires a clean installer revision'
        }
        $capabilityArgs = @('--', '--enable-windows-flash', '--source-commit', $revision)
    }'''
        self.assertIn('[switch]$EnableWindowsFlash', self.script)
        self.assertIn(gate, self.script)
        freeze = next(line for line in self.script.splitlines() if '-m PyInstaller ' in line)
        self.assertTrue(freeze.endswith('build/diskos-installer.spec @capabilityArgs'))
        self.assertLess(self.script.index(gate), self.script.index('python -I -m venv'))

    def test_native_gate_checks_ignored_inputs_before_and_after_freeze(self):
        root = Path(__file__).resolve().parents[1]
        vendor = root / 'vendor/windows-x86_64'
        code = re.search(r"\$nativeCode = @'\n(.*?)\n'@", self.script, re.S).group(1)
        tree = {}
        namespace = {}
        with mock.patch.object(sys, 'argv', ['verify', str(root), str(vendor), '0' * 64]), \
                mock.patch.object(sys, 'path', sys.path.copy()), \
                mock.patch('pathlib.Path.read_bytes', return_value=b''), \
                mock.patch.dict(sys.modules, {'deterministic_zip': types.SimpleNamespace(checked_stat=lambda p: None)}):
            with self.assertRaisesRegex(ValueError, 'retained tools-build hash'):
                exec(code, namespace)
        # Derive the recipe's input list; the fixture covers every generated output.
        recipe = (root / 'build/build-windows-tools.sh').read_text()
        inputs = re.search(r'\(cd "\$root" && sha256sum (.*?)\)', recipe, re.S).group(1)
        inputs = inputs.replace('\\\n', '').split()
        outputs = ('mksquashfs.exe', 'unsquashfs.exe', 'usbboot.exe', 'bind_winusb.exe', 'msys-2.0.dll')
        for base, names in ((root, inputs), (vendor, outputs)):
            for name in names:
                tree[base / name] = name.encode()
        for name in ('my_write6_dram.bin', 'disc_spl_lpddr3.bin'):
            tree[root / 'flash' / name] = tree[vendor / name] = name.encode()
        manifest = ('receipt\nInput SHA256 (paths relative to installer):\n'
                    + ''.join(hashlib.sha256(tree[root / n]).hexdigest() + '  ' + n + '\n' for n in inputs)
                    + '\nOutput SHA256:\n'
                    + ''.join(hashlib.sha256(tree[vendor / n]).hexdigest() + ' *' + n + '\n' for n in outputs))
        receipt = vendor / 'MANIFEST.txt'
        tree[receipt] = manifest.encode()
        expected = hashlib.sha256(tree[receipt]).hexdigest()
        def run():
            def read(path):
                if path not in tree:
                    raise FileNotFoundError(path)
                return tree[path]
            with mock.patch.object(sys, 'argv', ['verify', str(root), str(vendor), expected]), \
                    mock.patch.object(sys, 'path', sys.path.copy()), \
                    mock.patch('pathlib.Path.read_bytes', read), \
                    mock.patch.dict(sys.modules, {'deterministic_zip': types.SimpleNamespace(checked_stat=lambda p: None)}):
                exec(code, {})
        run()
        for path in list(tree):
            original = tree[path]
            tree[path] = original + b'changed'
            with self.subTest(path=path), self.assertRaises(ValueError):
                run()
            tree[path] = original
            del tree[path]
            with self.subTest(missing=path), self.assertRaises(FileNotFoundError):
                run()
            tree[path] = original
        tool = vendor / outputs[0]
        old_digest = hashlib.sha256(tree[tool]).hexdigest()
        tree[tool] += b'changed'
        tree[receipt] = manifest.replace(old_digest, hashlib.sha256(tree[tool]).hexdigest()).encode()
        with self.assertRaisesRegex(ValueError, 'retained tools-build hash'):
            run()
        for malformed in (manifest + manifest.split('Output SHA256:\n')[1],
                          manifest.replace(' *' + outputs[0], ' *../outside.exe'),
                          manifest.replace(' *' + outputs[0], ' *missing.exe')):
            tree[receipt] = malformed.encode()
            expected = hashlib.sha256(tree[receipt]).hexdigest()
            with self.assertRaises(ValueError):
                run()
        self.assertIn("$EnableWindowsFlash -and -not $PSBoundParameters.ContainsKey('NativeManifestSha256')", self.script)
        before = self.script.index('python -I -c $nativeCode $sourceRoot $nativeVendor $nativeHash')
        freeze = self.script.index('-m PyInstaller ')
        after = self.script.index("& $buildPython -I -c $nativeCode $sourceRoot (Join-Path $onedir")
        self.assertLess(before, self.script.index('python -I -m venv'))
        self.assertLess(freeze, after)
        self.assertLess(after, self.script.index('$stageCode ='))

    def test_runtime_sources_are_authenticated_before_and_after_freeze(self):
        verifier = self.script.split('    $verifyRuntimeSources = {\n', 1)[1].split('\n    }\n', 1)[0]
        self.assertIn("& 'C:\\msys64\\usr\\bin\\env.exe' 'PATH=/usr/bin:/bin' /usr/bin/bash --noprofile --norc -p build/fetch-windows-sources.sh --verify", verifier)
        self.assertIn("[Environment]::SetEnvironmentVariable('MSYS2_GNUPGHOME', '/etc/pacman.d/gnupg', 'Process')", verifier)
        self.assertIn("if ($LASTEXITCODE -ne 0) { throw 'Windows runtime source authentication failed' }", verifier)
        self.assertIn("} finally {\n            [Environment]::SetEnvironmentVariable('MSYS2_GNUPGHOME', $savedKeyring, 'Process')", verifier)
        before = self.script.index("& $verifyRuntimeSources 'corresponding-source/windows'")
        after = self.script.index("& $verifyRuntimeSources (Join-Path $onedir '_internal/corresponding-source/windows')")
        self.assertLess(self.script.index("throw 'Windows flash capability requires a clean installer revision'"), before)
        self.assertLess(before, self.script.index('python -I -m venv'))
        self.assertLess(self.script.index('-m PyInstaller '), after)
        self.assertLess(after, self.script.index('$stageCode ='))

    def test_build_uses_a_fresh_environment_and_freeze_outputs(self):
        self.assertIn("$run = Join-Path 'build/dist/windows' ([guid]::NewGuid().ToString('N'))", self.script)
        self.assertIn('New-Item -ItemType Directory -Path $run', self.script)
        for name in ('venv', 'dist', 'work'):
            self.assertIn(f"${name} = Join-Path $run '{name}'", self.script)
        self.assertIn('python -I -m venv $venv\n    if ($LASTEXITCODE -ne 0)', self.script)
        self.assertNotIn('Test-Path "$venv/Scripts/python.exe"', self.script)
        self.assertIn('--distpath $dist --workpath $work', self.script)
        self.assertNotIn('--system-site-packages', self.script)
        self.assertNotIn('Remove-Item', self.script)

    def test_install_is_only_from_the_hash_locked_wheels(self):
        installs = [line.strip() for line in self.script.splitlines() if ' pip ' in line and ' install ' in line]
        self.assertEqual(installs, [
            '& $buildPython -I -m pip --isolated --disable-pip-version-check install --no-cache-dir @installArgs'])
        self.assertIn("$installArgs = @('--require-hashes', '--only-binary=:all:', '-r', 'build/windows-build.lock')",
                      self.script)
        self.assertNotIn('requirements.txt', self.script)
        for bypass in ('--upgrade', '--no-deps', '--trusted-host'):
            self.assertNotIn(bypass, self.script)
        self.assertIn("if ($LASTEXITCODE -ne 0) { throw 'Could not install build requirements' }", self.script)
        self.assertLess(self.script.index(' install '), self.script.index('-m PyInstaller '))

    def test_offline_wheelhouse_has_no_index_fallback(self):
        self.assertIn('[ValidateNotNullOrEmpty()][string]$Wheelhouse', self.script)
        self.assertIn("$PSBoundParameters.ContainsKey('Wheelhouse')", self.script)
        self.assertIn('(Resolve-Path -LiteralPath $Wheelhouse).ProviderPath', self.script)
        self.assertIn('Test-Path -LiteralPath $wheelhousePath -PathType Container', self.script)
        self.assertLess(self.script.index('Resolve-Path -LiteralPath $Wheelhouse'),
                        self.script.index('Push-Location'))
        self.assertIn("if ($wheelhousePath) {\n        $installArgs += @('--no-index', '--find-links', $wheelhousePath)\n    }",
                      self.script)

    def test_libusb_is_an_explicit_verified_local_input_outside_cleaned_workpath(self):
        self.assertIn('[Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$LibusbDll', self.script)
        self.assertLess(self.script.index('Resolve-Path -LiteralPath $LibusbDll'),
                        self.script.index('Push-Location'))
        self.assertIn("Join-Path $PSScriptRoot 'windows-libusb.json'", self.script)
        self.assertIn('(Get-Item -LiteralPath $libusbPath).Length -ne $libusbPin.dll_size', self.script)
        self.assertIn('(Get-FileHash -LiteralPath $libusbPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $libusbPin.dll_sha256', self.script)
        self.assertLess(self.script.index("throw 'LibusbDll does not match"), self.script.index('python -I -m venv'))
        self.assertIn("$libusbInput = Join-Path $work 'libusb'", self.script)
        self.assertIn("Copy-Item -LiteralPath $libusbPath -Destination (Join-Path $libusbInput 'libusb-1.0.dll')", self.script)
        self.assertLess(self.script.index('Copy-Item -LiteralPath $libusbPath'), self.script.index('-m PyInstaller '))

    def test_python_startup_is_isolated_and_environment_is_restored(self):
        setup = '''    foreach ($name in @('PYTHONPATH', 'PYTHONHOME', 'PYTHONSTARTUP')) {
        $savedPythonEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }'''
        self.assertIn('$savedPythonEnv = @{}\ntry {\n' + setup, self.script)
        calls = re.findall(
            r'^[ \t]*(?:\$\w+[ \t]*=[ \t]*)?'
            r'((?:python|&[ \t]+(?:python|\$buildPython))[ \t]+[^\r\n]+)$',
            self.script, re.M)
        self.assertEqual(len(calls), 12)
        for call in calls:
            self.assertRegex(call, r'^(?:python|&[ \t]+(?:python|\$buildPython))[ \t]+-I[ \t]')
        self.assertLess(self.script.index(setup), self.script.index('python -I -c'))
        self.assertTrue(self.script.endswith('''} finally {
    foreach ($name in $savedPythonEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedPythonEnv[$name], 'Process')
    }
    Pop-Location
}
'''))

    def test_pip_environment_and_all_config_files_are_disabled_for_install(self):
        isolated_install = '''    $savedPipEnv = @{}
    try {
        foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
            if ($entry.Key -like 'PIP_*') {
                $savedPipEnv[$entry.Key] = $entry.Value
                [Environment]::SetEnvironmentVariable($entry.Key, $null, 'Process')
            }
        }
        # NUL disables all pip config files on Windows, including global/site config.
        [Environment]::SetEnvironmentVariable('PIP_CONFIG_FILE', 'NUL', 'Process')
        & $buildPython -I -m pip --isolated --disable-pip-version-check install --no-cache-dir @installArgs
        if ($LASTEXITCODE -ne 0) { throw 'Could not install build requirements' }
    } finally {
        [Environment]::SetEnvironmentVariable('PIP_CONFIG_FILE', $null, 'Process')
        foreach ($name in $savedPipEnv.Keys) {
            [Environment]::SetEnvironmentVariable($name, $savedPipEnv[$name], 'Process')
        }
    }'''
        self.assertIn(isolated_install, self.script)
        self.assertEqual(self.script.count('--find-links'), 1)
        self.assertNotIn('--index-url', self.script)
        self.assertNotIn('--extra-index-url', self.script)
        self.assertLess(self.script.index(isolated_install), self.script.index('-m PyInstaller '))

    def test_stage_is_complete_and_inventory_has_exact_byte_format(self):
        stage = re.search(r"\$stageCode = @'\n(.*?)\n'@", self.script, re.S)
        self.assertIsNotNone(stage, 'Missing onedir staging and inventory code')
        code = stage.group(1)
        compile(code, 'build-windows.ps1 stage code', 'exec')
        self.assertIn('checked_stat(onedir)', code)
        self.assertIn('shutil.copytree(onedir, stage, symlinks=True)', code)
        self.assertIn("executables = ('diskos-installer.exe', 'diskos-installer-cli.exe')", code)
        self.assertIn('all((onedir / name).is_file() for name in executables)', code)
        self.assertIn("(onedir / '_internal').is_dir()", code)
        self.assertIn("('LICENSE', 'NOTICE.md', 'README.md', 'licenses', 'src', 'build', 'corresponding-source')", code)
        self.assertIn("'RECOVERY.txt', 'build/windows-build.lock'", code)
        self.assertIn('os.walk(stage, followlinks=False, onerror=fail)', code)
        self.assertIn('checked_stat(Path(folder) / name)', code)
        self.assertIn('relative = path.relative_to(stage).as_posix()', code)
        self.assertIn("if relative == 'SHA256SUMS':\n            raise ValueError", code)
        self.assertIn("hashlib.file_digest(stream, 'sha256').hexdigest()", code)
        self.assertIn("digest + '  ' + name + '\\n' for name, digest in sorted(records)", code)
        self.assertIn("(stage / 'SHA256SUMS').write_bytes(inventory.encode('utf-8'))", code)
        self.assertIn('set(executables).issubset(name for name, digest in records)', code)
        self.assertNotIn('SLICE 8b:', self.script)
        self.assertIn("$onedir = Join-Path $dist 'diskos-installer'", self.script)
        self.assertLess(self.script.index("throw 'Onedir build failed'"), self.script.index('$stageCode ='))
        self.assertIn("if ($LASTEXITCODE -ne 0) { throw 'Could not stage onedir and SHA256 inventory' }", self.script)

    def test_stage_requires_and_inventories_both_executables(self):
        code = re.search(r"\$stageCode = @'\n(.*?)\n'@", self.script, re.S).group(1)
        names = ('diskos-installer.exe', 'diskos-installer-cli.exe')
        for missing in (None, *names):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as work:
                onedir, stage = Path(work) / 'onedir', Path(work) / 'stage'
                internal = onedir / '_internal'
                internal.mkdir(parents=True)
                for name in names:
                    if name != missing:
                        (onedir / name).write_bytes(name.encode('ascii'))
                for name in ('LICENSE', 'NOTICE.md', 'README.md', 'licenses/notice.txt',
                             'src/source.c', 'build/recipe.sh', 'corresponding-source/source.txt',
                             'vendor/windows-x86_64/MANIFEST.txt'):
                    path = internal / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'fixture')
                with mock.patch.object(sys, 'argv', ['stage', str(onedir), str(stage), 'False', 'default']), \
                        mock.patch.object(sys, 'path', sys.path.copy()), \
                        mock.patch('importlib.metadata.version', return_value='fixture'):
                    if missing:
                        with self.assertRaisesRegex(ValueError, 'complete Windows onedir'):
                            exec(compile(code, 'Windows staging', 'exec'), {})
                        self.assertFalse(stage.exists())
                        continue
                    exec(compile(code, 'Windows staging', 'exec'), {})
                inventory = (stage / 'SHA256SUMS').read_text(encoding='utf-8').splitlines()
                for name in names:
                    self.assertEqual((stage / name).read_bytes(), name.encode('ascii'))
                    digest = hashlib.sha256((stage / name).read_bytes()).hexdigest()
                    self.assertEqual(inventory.count(digest + '  ' + name), 1)

    def test_two_zips_are_checked_before_reporting_the_final_hash(self):
        commands = [line.strip() for line in self.script.splitlines() if ' build/deterministic_zip.py ' in line]
        self.assertEqual(commands, [
            '& $buildPython -I build/deterministic_zip.py $stage $zip',
            '& $buildPython -I build/deterministic_zip.py $stage $checkZip'])
        self.assertIn("$zip = Join-Path $run 'diskos-installer-windows-x86_64.zip'", self.script)
        self.assertIn("$checkZip = Join-Path $run 'diskos-installer-windows-x86_64.check.zip'", self.script)
        self.assertIn("$stage = Join-Path $run 'diskos-installer-windows-x86_64'", self.script)
        for command, diagnostic in zip(commands, ('final', 'verification')):
            self.assertIn(command + f"\n    if ($LASTEXITCODE -ne 0) {{ throw 'Could not build {diagnostic} ZIP' }}",
                          self.script)
        for path in ('$zip', '$checkZip'):
            self.assertIn(f'(Get-FileHash -LiteralPath {path} -Algorithm SHA256).Hash.ToLowerInvariant()', self.script)
        comparison = "if ($zipHash -ne $checkHash) { throw 'ZIP SHA256 mismatch: archive is not deterministic' }"
        self.assertIn(comparison, self.script)
        self.assertLess(self.script.index("throw 'Could not stage onedir"), self.script.index(commands[0]))
        self.assertLess(self.script.index(commands[1]), self.script.index('$zipHash ='))
        self.assertLess(self.script.index(comparison), self.script.index('Write-Host "ZIP:'))
        self.assertIn('Write-Host "ZIP: $((Resolve-Path -LiteralPath $zip).ProviderPath)"', self.script)
        self.assertIn('Write-Host "SHA256: $zipHash', self.script)


class WindowsOutputTests(unittest.TestCase):
    def test_remove_completion_text_per_platform(self):
        windows_note = ("The boot-mode driver choice stays. To undo: Device Manager > "
                        "boot-mode device > Uninstall device; leave the driver-package box unticked.")
        other_note = "Nothing was installed system-wide, so there is nothing else to clean up."
        for host in ('windows', 'linux', 'macos'):
            with self.subTest(host=host), \
                    mock.patch.object(cli.platform_probe, 'host', return_value=(host, 'x86_64')), \
                    mock.patch.object(cli.service, 'do_remove', return_value={'ok': True}), \
                    mock.patch.object(cli.ui, 'info') as info:
                self.assertEqual(cli.cmd_remove(types.SimpleNamespace(yes=True, force=False)), 0)
                text = info.call_args.args[0]
                self.assertEqual(text, windows_note if host == 'windows' else other_note)
                self.assertTrue(text.isascii())

    def render(self, stream, tty=False):
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream), mock.patch.object(ui, '_TTY', tty):
            ui.step('Build \u97f3')
            ui.ok('Saved \u00e9')
            ui.warn('Path \U0001f3b5')
            ui.err('Failed \u97f3')
            bar = ui.Bar('Work \u97f3', 4)
            bar.update(2, '\U0001f3b5')
            bar.done()
            heartbeat = ui.Heartbeat('Flash \u97f3')
            heartbeat.tick()
            heartbeat.stop()
        stream.flush()

    def test_cp1252_output_and_progress(self):
        for tty in (False, True):
            with self.subTest(tty=tty):
                raw = io.BytesIO()
                stream = io.TextIOWrapper(raw, encoding='cp1252', errors='strict')
                self.render(stream, tty)
                text = re.sub(r'\x1b\[[0-9;]*m', '', raw.getvalue().decode('cp1252'))
                self.assertIn('> ', text)
                self.assertIn('OK Saved \u00e9', text)
                self.assertIn('X ', text)
                self.assertIn(r'\u97f3', text)
                self.assertIn(r'\U0001f3b5', text)
                if tty:
                    self.assertIn('#', text)
                    self.assertIn('*', text)
                stream.close()

    def test_utf8_keeps_symbols_and_paths(self):
        raw = io.BytesIO()
        stream = io.TextIOWrapper(raw, encoding='utf-8', errors='strict')
        self.render(stream, True)
        text = raw.getvalue().decode('utf-8')
        for char in ('\u25b6', '\u2713', '\u2717', '\u2588', '\u97f3', '\U0001f3b5'):
            self.assertIn(char, text)
        self.assertTrue(any(char in text for char in ui.Heartbeat.FRAMES))
        stream.close()

    def test_cp864_progress_bar(self):
        for tty in (False, True):
            with self.subTest(tty=tty):
                raw = io.BytesIO()
                with io.TextIOWrapper(raw, encoding='cp864', errors='strict') as stream:
                    with contextlib.redirect_stdout(stream), mock.patch.object(ui, '_TTY', tty):
                        bar = ui.Bar('Work', 4)
                        bar.update(2)
                        bar.done()
                    stream.flush()
                    text = raw.getvalue().decode('cp864')
                    self.assertIn('0?', text)
                    self.assertIn('100?', text)
                    self.assertNotIn('%', text)

    def test_strict_codecs_accept_all_output(self):
        text = (''.join(chr(code) for code in range(128))
                + ''.join(ui._ASCII_SYMBOLS) + '\u00e9\u97f3\U0001f3b5\udcff')
        for encoding in ('ascii', 'cp1252', 'cp864', 'cp037', 'shift_jis', 'utf-8'):
            for tty in (False, True):
                with self.subTest(encoding=encoding, tty=tty):
                    rendered = ui.output_text(text, encoding)
                    rendered.encode(encoding, 'strict')
                    for char in text:
                        try:
                            char.encode(encoding, 'strict')
                        except UnicodeEncodeError:
                            candidate = ui._ASCII_SYMBOLS.get(
                                char, char.encode('ascii', 'backslashreplace').decode('ascii'))
                            expected = candidate.encode(encoding, 'replace').decode(encoding)
                        else:
                            expected = char
                        self.assertEqual(ui.output_text(char, encoding), expected)
                    raw = io.BytesIO()
                    with io.TextIOWrapper(raw, encoding=encoding, errors='strict') as stream:
                        self.assertEqual(ui.SafeOutput(stream).write(text), len(text))
                        self.render(stream, tty)
                        self.assertTrue(raw.getvalue())

    def test_redirected_cp1252_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'install.log'
            with path.open('w', encoding='cp1252', errors='strict') as stream:
                self.render(stream)
            text = path.read_bytes().decode('cp1252')
            self.assertIn('> Build', text)
            self.assertIn('OK Saved', text)
            self.assertNotIn('\u2713', text)

    def test_ascii_output_handles_surrogates(self):
        raw = io.BytesIO()
        stream = io.TextIOWrapper(raw, encoding='ascii', errors='strict')
        with contextlib.redirect_stdout(stream):
            ui.ok('path \udcff \u00e9')
        self.assertEqual(raw.getvalue().decode('ascii'), 'OK path \\udcff \\xe9' + os.linesep)
        stream.close()

    def test_stream_with_no_encoding(self):
        class StrictStream:
            def write(self, text):
                text.encode('ascii')
                self.text = text
        stream = StrictStream()
        ui.SafeOutput(stream).write('\u2713 \u97f3')
        self.assertEqual(stream.text, 'OK \\u97f3')

    def test_stream_reports_wrong_encoding(self):
        class StrictStream:
            encoding = 'utf-8'
            def write(self, text):
                text.encode('cp1252')
                self.text = text
        stream = StrictStream()
        self.assertEqual(ui.SafeOutput(stream).write('\u2713 \u97f3'), 3)
        self.assertEqual(stream.text, 'OK \\u97f3')

    def test_parser_error_redirected_in_subprocess(self):
        # Like cmd redirection: both inherited streams point at a real file.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'cli.log'
            env = dict(os.environ, PYTHONIOENCODING='cp1252:strict')
            with path.open('wb') as stream:
                result = subprocess.run([sys.executable, '-m', 'diskos_installer', 'install', '--bad-\u97f3'],
                                        cwd=Path(__file__).resolve().parents[1], env=env,
                                        stdout=stream, stderr=stream)
            text = path.read_bytes().decode('cp1252')
            self.assertEqual(result.returncode, 2)
            self.assertIn(r'--bad-\u97f3', text)
            self.assertNotIn('Traceback', text)

    def test_string_exit_uses_safe_stderr(self):
        raw = io.BytesIO()
        stream = io.TextIOWrapper(raw, encoding='cp1252', errors='strict')
        with contextlib.redirect_stderr(stream), mock.patch.object(cli, '_main', side_effect=SystemExit('path \u97f3')):
            self.assertEqual(cli.main(['install']), 1)
        self.assertIn(r'path \u97f3', raw.getvalue().decode('cp1252'))
        stream.close()


class FirmwareDependencyTests(unittest.TestCase):
    def missing_crypto(self, exc=None):
        original = builtins.__import__
        def importing(name, *args, **kwargs):
            if name == 'Crypto.Cipher':
                raise exc or ModuleNotFoundError("No module named 'Crypto'")
            return original(name, *args, **kwargs)
        return mock.patch('builtins.__import__', side_effect=importing)

    def test_missing_import_and_native_backend_are_coded(self):
        for exc in (ModuleNotFoundError("No module named 'Crypto'"), OSError('missing AES DLL')):
            with self.subTest(exc=exc), self.missing_crypto(exc), self.assertRaises(imagebuild.BuildError) as caught:
                service.require_firmware_dependencies()
            self.assertEqual(caught.exception.code, 'E105')
            self.assertIn('requirements.txt', str(caught.exception))

    def test_aes_backend_is_exercised(self):
        aes = mock.Mock()
        with mock.patch.dict(sys.modules, {'Crypto.Cipher': types.SimpleNamespace(AES=aes)}):
            service.require_firmware_dependencies()
        aes.new.assert_called_once_with(bytes(32), aes.MODE_CBC, bytes(16))

    def test_missing_native_backend_on_cipher_creation(self):
        aes = mock.Mock()
        aes.new.side_effect = OSError('missing AES DLL')
        with mock.patch.dict(sys.modules, {'Crypto.Cipher': types.SimpleNamespace(AES=aes)}), self.assertRaises(imagebuild.BuildError) as caught:
            service.require_firmware_dependencies()
        self.assertEqual(caught.exception.code, 'E105')

    def test_install_and_restore_stop_before_work_or_device(self):
        for flow in (service.do_install, service.do_restore):
            with self.subTest(flow=flow.__name__), self.missing_crypto(), mock.patch.object(service.state, 'build_dir') as work, \
                    mock.patch.object(service.state, 'save') as save, mock.patch.object(service.bundle, 'data') as data, \
                    mock.patch.object(imagebuild, 'extract_stock_rootfs') as extract, \
                    mock.patch.object(service.flasher, 'preflight') as preflight, mock.patch.object(service.flasher, 'flash') as flash:
                confirm = mock.Mock()
                with self.assertRaises(imagebuild.BuildError) as caught:
                    flow({'firmware': 'stock.zip'}, Reporter(), confirm)
                self.assertEqual(caught.exception.code, 'E105')
                for action in (work, save, data, extract, preflight, flash, confirm):
                    action.assert_not_called()

    def test_stock_input_does_not_require_crypto(self):
        with mock.patch.object(service, 'require_firmware_dependencies') as check, mock.patch.object(service.bundle, 'data', return_value=None):
            with self.assertRaises(imagebuild.BuildError) as caught:
                service.do_install({'stock': 'stock.sq', 'firmware': 'ignored.zip'}, Reporter(), mock.Mock())
            self.assertEqual(caught.exception.code, 'E102')
            check.assert_not_called()

    def test_cli_missing_dependency_returns_error_without_traceback(self):
        for command in ('install', 'restore-stock'):
            raw = io.BytesIO()
            stream = io.TextIOWrapper(raw, encoding='cp1252', errors='strict')
            with self.subTest(command=command), self.missing_crypto(), contextlib.redirect_stderr(stream), \
                    mock.patch.object(cli, '_Lock', return_value=contextlib.nullcontext()), \
                    mock.patch.object(service.flasher, 'flash_eligible', return_value=True), \
                    mock.patch.object(service.flasher, 'preflight') as preflight:
                self.assertEqual(cli.main([command, '--firmware', 'stock.zip', '--yes']), 1)
                preflight.assert_not_called()
            text = raw.getvalue().decode('cp1252')
            self.assertIn('[E105]', text)
            self.assertIn('requirements.txt', text)
            self.assertNotIn('Traceback', text)
            stream.close()

    def test_cli_default_off_windows_rejects_before_missing_dependency(self):
        cap = types.SimpleNamespace(WINDOWS_FLASH=False, SOURCE_COMMIT='')
        stream = io.StringIO()
        with self.missing_crypto(), contextlib.redirect_stderr(stream), \
                mock.patch.object(cli, '_Lock', return_value=contextlib.nullcontext()), \
                mock.patch.object(service.flasher, '_build_capabilities', cap, create=True), \
                mock.patch.object(sys, 'frozen', True, create=True), \
                mock.patch.object(cli.platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(service, 'require_firmware_dependencies') as dependencies, \
                mock.patch.object(service.flasher, 'preflight') as preflight:
            self.assertEqual(cli.main(['install', '--firmware', 'stock.zip', '--yes']), 2)
            dependencies.assert_not_called()
            preflight.assert_not_called()
        text = stream.getvalue()
        self.assertIn('[E101]', text)
        self.assertIn('flashing not yet supported', text)
        self.assertNotIn('[E105]', text)
        self.assertNotIn('Traceback', text)

    def run_spec(self, work, args=(), version='1.2.3', native=True, git_outputs=None,
                 host=('Windows', 'AMD64'), missing=(), libfiles=(),
                 licence_text='upstream licence fixture\n', spec_source=None,
                 dll_bytes=b'pinned DLL fixture', dll_missing=False, analysis_binaries=None,
                 analysis_datas=None, post_analysis_bytes=None, pin=None,
                 collection_binaries=(), collection_datas=(), collection_stage='hooks',
                 final_binaries=None, final_datas=None, tk_collected=()):
        root = Path(__file__).resolve().parents[1]
        dll = Path(work).parent / 'libusb/libusb-1.0.dll'
        # Synthetic bytes keep the mechanism tests independent of local build inputs.
        # The separate evidence test compares the real manifest to the official DLL.
        manifest = json.loads((root / 'build/windows-libusb.json').read_text())
        manifest.update(dll_sha256=hashlib.sha256(dll_bytes).hexdigest(), dll_size=len(dll_bytes))
        manifest.update(pin or {})
        real_open = builtins.open
        real_isdir = os.path.isdir
        real_glob = Path.glob
        mac_inputs = types.ModuleType('macos_inputs')
        mac_inputs.verify = mock.Mock()
        mac_libs = sorted(set(libfiles) | {'libusb-1.0.0.dylib'})
        def globbing(path, pattern):
            if host[0] == 'Darwin' and path == root / 'vendor/macos-arm64/lib':
                return iter(path / name for name in mac_libs if name.endswith('.dylib'))
            return real_glob(path, pattern)
        analysed = False
        def opening(path, *args, **kwargs):
            if str(path) == str(root / 'licenses/PyInstaller-COPYING.txt'):
                return io.StringIO(licence_text)
            if str(path) == str(root / 'build/windows-libusb.json'):
                return io.StringIO(json.dumps(manifest))
            if str(path) == str(dll):
                if dll_missing:
                    raise FileNotFoundError(path)
                return io.BytesIO(post_analysis_bytes if analysed and post_analysis_bytes is not None else dll_bytes)
            if str(path) in (str(Path(work) / 'windows-hooks/hook-usb.py'),
                              str(Path(work) / 'macos-hooks/hook-usb.py')):
                if not real_isdir(Path(path).parent):
                    Path(path).parent.mkdir()
            return real_open(path, *args, **kwargs)
        repository_commands = [
            ('rev-parse', '--show-toplevel'),
            ('rev-parse', '--verify', 'HEAD^{commit}'),
            ('status', '--porcelain', '--untracked-files=no'),
            ('ls-files', '--others', '--exclude-standard', '-z', '--',
             'diskos_installer', 'payload', 'vendor/windows-x86_64', 'src/usbboot'),
        ]
        outputs = dict(zip(repository_commands, (str(root) + '\n', 'a' * 40 + '\n', '', '')))
        outputs.update(git_outputs or {})
        def git_run(command, **kwargs):
            self.assertEqual(command[:4], ['git', '--no-optional-locks', '-C', str(root)])
            self.assertTrue(kwargs['check'])
            self.assertEqual(kwargs['stdout'], subprocess.PIPE)
            self.assertEqual(kwargs['stderr'], subprocess.PIPE)
            result = outputs[tuple(command[4:])]
            if isinstance(result, Exception):
                raise result
            return subprocess.CompletedProcess(command, 0, stdout=result, stderr='')
        hooks = types.ModuleType('PyInstaller.utils.hooks')
        crypto_modules = [
            'Crypto', 'Crypto.Cipher.AES', 'Crypto.Cipher._raw_aes', 'Crypto.Util._raw_api',
            'Crypto.SelfTest', 'Crypto.SelfTest.Cipher', 'Crypto.SelfTest.Cipher.test_AES',
            'Crypto.SelfTestOther',
        ]
        def collect_submodules(package, filter=lambda name: True):
            return [name for name in crypto_modules if filter(name)]
        hooks.collect_submodules = mock.Mock(side_effect=collect_submodules)
        hooks.collect_data_files = mock.Mock(
            return_value=[(str(root / 'site-packages/certifi/cacert.pem'), 'certifi')])
        build_main = types.ModuleType('PyInstaller.building.build_main')
        build_main.normalize_toc = windows_normalize_toc
        building = types.ModuleType('PyInstaller.building')
        building.build_main = build_main
        cli_script = ('__main__', str(root / 'diskos_installer/__main__.py'), 'PYSOURCE')
        gui_script = ('gui_entry', str(root / 'diskos_installer/gui_entry.py'), 'PYSOURCE')
        shared_scripts = [
            ('diskos_build_capabilities', str(Path(work) / 'diskos_build_capabilities.py'), 'PYSOURCE'),
            ('pyi_rth_inspect', str(Path(work) / 'pyi_rth_inspect.py'), 'PYSOURCE'),
        ]
        scripts = shared_scripts + [cli_script] + ([gui_script] if host[0] == 'Windows' else [])
        frozen = types.SimpleNamespace(pure=['pure'], scripts=scripts,
                                       binaries=analysis_binaries if analysis_binaries is not None else (
                                           [('libusb-1.0.dll', str(dll), 'BINARY')] if host[0] == 'Windows' else ['binary']),
                                       datas=analysis_datas if analysis_datas is not None else [])
        def analysed_result(*args, **kwargs):
            nonlocal analysed
            if host[0] == 'Windows':
                # Spec inputs, then hook collection before build_main.py:745.
                frozen.binaries = build_main.normalize_toc(frozen.binaries)
                frozen.datas = build_main.normalize_toc(frozen.datas)
                for stage in ('hooks', 'ctypes', 'dependencies'):
                    if stage == collection_stage:
                        frozen.binaries += list(collection_binaries)
                        frozen.datas += list(collection_datas)
                    if stage == 'ctypes':
                        # Separate normalizations at build_main.py:824-825.
                        frozen.datas = build_main.normalize_toc(frozen.datas)
                        frozen.binaries = build_main.normalize_toc(frozen.binaries)
                        continue
                    # Combined reclassification at :745 and :1009.
                    combined = build_main.normalize_toc(frozen.datas + frozen.binaries)
                    frozen.binaries = [entry for entry in combined if entry[2] in ('BINARY', 'EXTENSION')]
                    frozen.datas = [entry for entry in combined if entry[2] not in ('BINARY', 'EXTENSION')]
                if final_binaries is not None:
                    frozen.binaries = final_binaries
                if final_datas is not None:
                    frozen.datas = final_datas
            analysed = True
            return frozen
        def collected_result(*args, **kwargs):
            return types.SimpleNamespace(toc=list(args[-2]) + list(args[-1]) + list(tk_collected))
        analysis, collect, pyz = (mock.Mock(side_effect=analysed_result),
                                  mock.Mock(side_effect=collected_result), mock.Mock())
        outputs_exe = [object(), object()] if host[0] == 'Windows' else [object()]
        exe = mock.Mock(side_effect=outputs_exe)
        spec = root / 'build/diskos-installer.spec'
        # Match building.build_main.build() in the pinned PyInstaller 6.16.0.
        # Plain exec adds builtins; runpy adds globals the real builder lacks.
        namespace = {
            'DISTPATH': str(Path(work) / 'dist'),
            'HOMEPATH': str(Path(work) / 'PyInstaller'),
            'SPEC': str(spec),
            'specnm': spec.stem,
            'SPECPATH': str(spec.parent),
            'WARNFILE': str(Path(work) / 'warn-diskos-installer.txt'),
            'workpath': work,
            'TOC': mock.Mock(),
            'Analysis': analysis,
            'BUNDLE': mock.Mock(),
            'COLLECT': collect,
            'EXE': exe,
            'MERGE': mock.Mock(),
            'PYZ': pyz,
            'Tree': mock.Mock(),
            'Splash': mock.Mock(),
            'os': os,
        }
        self.assertEqual(set(namespace), {
            'DISTPATH', 'HOMEPATH', 'SPEC', 'specnm', 'SPECPATH', 'WARNFILE',
            'workpath', 'TOC', 'Analysis', 'BUNDLE', 'COLLECT', 'EXE', 'MERGE',
            'PYZ', 'Tree', 'Splash', 'os',
        })
        code = compile(spec.read_bytes() if spec_source is None else spec_source, str(spec), 'exec')
        absent = {str(root / name) for name in missing}
        is_file = os.path.isfile
        present = lambda path: str(path) not in absent if Path(path).is_relative_to(root) else is_file(path)
        with mock.patch.dict(sys.modules, {'PyInstaller.utils.hooks': hooks,
                'PyInstaller.building': building, 'PyInstaller.building.build_main': build_main,
                'macos_inputs': mac_inputs}), \
                mock.patch.object(Path, 'glob', globbing), \
                mock.patch('platform.system', return_value=host[0]), mock.patch('platform.machine', return_value=host[1]), \
                mock.patch('os.getcwd', return_value=str(root)), mock.patch('os.path.exists', side_effect=present), \
                mock.patch('os.path.isfile', side_effect=present), mock.patch('os.path.isdir', return_value=bool(libfiles)), \
                mock.patch('os.listdir', return_value=libfiles), \
                mock.patch('os.makedirs'), \
                mock.patch('builtins.open', side_effect=opening), \
                mock.patch('subprocess.run', side_effect=git_run) as git, \
                mock.patch.object(verify_git_blobs, 'BlobGuard') as blob_guard, \
                mock.patch.object(sys, 'argv', ['diskos-installer.spec', *args]), \
                mock.patch.object(sys, 'platform', 'win32' if native else 'cygwin'), \
                mock.patch('diskos_installer.__version__', version), \
                mock.patch.object(service, 'require_firmware_dependencies') as check:
            try:
                exec(code, namespace)
            finally:
                self.assertIs(build_main.normalize_toc, windows_normalize_toc)
        self.assertEqual([tuple(call.args[0][4:]) for call in git.call_args_list],
                         repository_commands if '--enable-windows-flash' in args else [])
        blob_guard.assert_called_once_with(str(root),
                                           'a' * 40 if '--enable-windows-flash' in args else 'HEAD')
        blob_guard.return_value.verify.assert_called_once()
        pairs = blob_guard.return_value.verify.call_args.args[0]
        for name in verify_git_blobs.PAYLOAD:
            self.assertIn((root / 'payload' / name, 'payload/' + name), pairs)
        for name in verify_git_blobs.DEVICE:
            self.assertIn((root / 'vendor' / namespace['TAG'] / name,
                           'flash/' + name), pairs)
        check.assert_called_once_with()
        hooks.collect_submodules.assert_called_once()
        self.assertEqual(hooks.collect_submodules.call_args.args, ('Crypto',))
        self.assertIn('Crypto.Cipher.AES', analysis.call_args.kwargs['hiddenimports'])
        analysis.assert_called_once()
        expected_entries = [cli_script[1]] + ([gui_script[1]] if host[0] == 'Windows' else [])
        self.assertEqual(analysis.call_args.args[0], expected_entries)
        pyz.assert_called_once_with(frozen.pure)
        if host[0] == 'Windows':
            self.assertEqual(analysis.call_args.kwargs['binaries'], [(str(dll), '.')])
            self.assertEqual(analysis.call_args.kwargs['hookspath'], [str(Path(work) / 'windows-hooks')])
            hook_namespace = {}
            exec((Path(work) / 'windows-hooks/hook-usb.py').read_text(), hook_namespace)
            self.assertEqual(hook_namespace['binaries'], [])
            self.assertEqual(hook_namespace['hiddenimports'], ['glob'])
            self.assertEqual(exe.call_count, 2)
            for call, entry, name, console in zip(exe.call_args_list, (gui_script, cli_script),
                    ('diskos-installer', 'diskos-installer-cli'), (False, True)):
                self.assertEqual(call.args, (pyz.return_value, shared_scripts + [entry], []))
                self.assertEqual(call.kwargs['name'], name)
                self.assertIs(call.kwargs['console'], console)
                self.assertTrue(call.kwargs['exclude_binaries'])
                self.assertEqual(call.kwargs['contents_directory'], '_internal')
                self.assertFalse(call.kwargs['upx'])
                self.assertFalse(call.kwargs['strip'])
            collect.assert_called_once_with(*outputs_exe, frozen.binaries, frozen.datas,
                                            strip=False, upx=False, name='diskos-installer')
        elif host[0] == 'Darwin':
            mac_inputs.verify.assert_called_once()
            self.assertEqual(analysis.call_args.kwargs['binaries'], [
                *[(str(root / 'vendor/macos-arm64' / name), 'vendor/macos-arm64')
                  for name in ('usbboot', 'mksquashfs', 'unsquashfs')],
                *[(str(root / 'vendor/macos-arm64/lib' / name), 'vendor/macos-arm64/lib')
                  for name in mac_libs if name.endswith('.dylib')]])
            self.assertEqual(analysis.call_args.kwargs['hookspath'], [str(Path(work) / 'macos-hooks')])
            exe.assert_called_once_with(pyz.return_value, frozen.scripts, [], exclude_binaries=True,
                                       name='diskos-installer', debug=False, bootloader_ignore_signals=False,
                                       strip=False, upx=False, console=True, target_arch='arm64',
                                       codesign_identity='-', argv_emulation=False)
            collect.assert_called_once_with(outputs_exe[0], frozen.binaries, frozen.datas,
                                            strip=False, upx=False, name='diskos-installer')
            self.assertEqual(namespace['coll'].toc, frozen.binaries + frozen.datas)
            namespace['BUNDLE'].assert_called_once_with(namespace['coll'],
                name='diskOS Installer.app', bundle_identifier='org.diskos.installer',
                info_plist={'CFBundleShortVersionString': version, 'NSHighResolutionCapable': True},
                codesign_identity='-')
        else:
            self.assertEqual(analysis.call_args.kwargs['binaries'], [])
            self.assertEqual(analysis.call_args.kwargs['hookspath'], [])
            exe.assert_called_once()
            self.assertEqual(exe.call_args.kwargs['name'], 'diskos-installer')
            self.assertTrue(exe.call_args.kwargs['console'])
            collect.assert_not_called()
            self.assertEqual(exe.call_args.args,
                             (pyz.return_value, frozen.scripts, frozen.binaries, frozen.datas, []))
            self.assertNotIn('exclude_binaries', exe.call_args.kwargs)
        return analysis.call_args.kwargs

    def test_spec_refuses_unpinned_missing_and_changed_libusb(self):
        for case in ({'dll_missing': True}, {'pin': {'dll_sha256': '0' * 64}},
                     {'pin': {'dll_size': 0}}, {'post_analysis_bytes': b'changed DLL'},
                     {'pin': {'source_sha256': '0' * 64}}):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as work:
                with self.assertRaisesRegex(SystemExit, 'pinned libusb input'):
                    self.run_spec(work, **case)

    def test_spec_refuses_host_libusb_even_when_bytes_match(self):
        with tempfile.TemporaryDirectory() as work:
            pinned = ('libusb-1.0.dll', str(Path(work).parent / 'libusb/libusb-1.0.dll'), 'BINARY')
            stray = Path(work) / 'System32/LibUSB-1.0.DLL'
            stray.parent.mkdir()
            for content in (b'unknown host DLL', b'pinned DLL fixture'):
                stray.write_bytes(content)
                entry = ('libusb-1.0.dll', str(stray), 'BINARY')
                for entries in ([entry], [pinned, entry], [entry, pinned]):
                    with self.subTest(content=content, entries=entries):
                        with self.assertRaisesRegex(SystemExit, 'unexpected USB backend DLL'):
                            self.run_spec(work, analysis_binaries=entries)
            with self.assertRaisesRegex(SystemExit, 'exactly one pinned'):
                self.run_spec(work, analysis_binaries=[])
            self.run_spec(work, analysis_binaries=[pinned, pinned])
            for entry in (('vendor/libusb-1.0.dll', pinned[1], 'BINARY'),
                          ('renamed.dll', pinned[1], 'BINARY'),
                          ('libusb-1.0.dll', pinned[1], 'DATA')):
                with self.assertRaisesRegex(SystemExit, 'unexpected USB backend DLL'):
                    self.run_spec(work, analysis_binaries=[entry])
            with self.assertRaisesRegex(SystemExit, 'unexpected USB backend DLL'):
                self.run_spec(work, final_datas=[pinned])

    def test_spec_refuses_every_alternate_usb_backend_name(self):
        # Runtime candidates at pyi_rth_usb.py:79-85; Windows glob at :47.
        candidates = ('usb-1.0', 'libusb-1.0', 'usb', 'usb-0.1', 'libusb0',
                      'libusb', 'openusb', 'cygusb-1.0', 'cygusb0')
        for candidate in candidates:
            names = [candidate + suffix for suffix in ('.dll', '-host.dll', 'extra.DLL')]
            if candidate == 'cygusb-1.0':
                names.append('cygusb-1.0-0.dll')  # Exact collection-hook fallback.
            for name in names:
                if name == 'libusb-1.0.dll':
                    continue  # The conflicting canonical name has its own regression.
                for field in ('source', 'destination'):
                    for kind in ('BINARY', 'DATA'):
                        with self.subTest(name=name, field=field, kind=kind), \
                                tempfile.TemporaryDirectory() as work:
                            dest = 'nested/' + (name.upper() if field == 'destination' else 'renamed.dll')
                            source = 'C:\\Windows\\System32\\' + (name.upper() if field == 'source' else 'renamed.dll')
                            addition = [(dest, source, kind)]
                            with self.assertRaisesRegex(SystemExit, 'unexpected USB backend DLL'):
                                self.run_spec(work, collection_binaries=addition if kind == 'BINARY' else (),
                                              collection_datas=addition if kind == 'DATA' else ())

    def test_spec_refuses_conflict_before_normalization(self):
        with tempfile.TemporaryDirectory() as work:
            pinned = ('libusb-1.0.dll', str(Path(work).parent / 'libusb/libusb-1.0.dll'), 'BINARY')
            for stage in ('hooks', 'ctypes', 'dependencies'):
                for dest in ('libusb-1.0.dll', 'LIBUSB-1.0.DLL', './libusb-1.0.dll'):
                    for kind in ('BINARY', 'DATA'):
                        host = (dest, 'C:\\Windows\\System32\\libusb-1.0.dll', kind)
                        # Real normalization discards this conflict, retaining the pin.
                        raw = [host, pinned] if kind == 'DATA' else [pinned, host]
                        self.assertEqual(windows_normalize_toc(raw), [pinned])
                        with self.subTest(stage=stage, dest=dest, kind=kind), \
                                self.assertRaisesRegex(SystemExit, 'unexpected USB backend DLL'):
                            self.run_spec(work, collection_stage=stage,
                                          collection_binaries=[host] if kind == 'BINARY' else (),
                                          collection_datas=[host] if kind == 'DATA' else ())

    def test_spec_final_toc_check_is_retained(self):
        with tempfile.TemporaryDirectory() as work:
            pinned = ('libusb-1.0.dll', str(Path(work).parent / 'libusb/libusb-1.0.dll'), 'BINARY')
            for entries in ([], [pinned, pinned]):
                with self.subTest(entries=entries), self.assertRaisesRegex(SystemExit, 'exactly one pinned'):
                    self.run_spec(work, final_binaries=entries)
            for dest, source in (('libusb-1.0.dll', 'C:/Windows/System32/libusb-1.0.dll'),
                                 ('usb-host.dll', 'C:/Windows/System32/renamed.dll'),
                                 ('renamed.dll', 'C:/Windows/System32/openusb-host.dll')):
                with self.subTest(dest=dest, source=source), self.assertRaises(SystemExit):
                    self.run_spec(work, final_binaries=[pinned, (dest, source, 'BINARY')])

    def test_libusb_manifest_matches_official_local_evidence(self):
        root = Path(__file__).resolve().parents[1]
        pin = json.loads((root / 'build/windows-libusb.json').read_text())
        self.assertEqual(pin['dll_sha256'], 'd4e5db4fad8bef7201dc5a4a71ca997e6dccc9da25e8a988d4be63b13a02208d')
        self.assertEqual(pin['dll_size'], 157184)
        self.assertEqual(pin['member'], 'VS2022/MS64/dll/libusb-1.0.dll')
        source = (root / pin['source_path']).read_bytes()
        self.assertEqual(hashlib.sha256(source).hexdigest(), pin['source_sha256'])
        self.assertEqual(len(source), pin['source_size'])
        location = os.environ.get('DISKOS_WINDOWS_LIBUSB_EVIDENCE')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_LIBUSB_EVIDENCE to the official archive/extraction directory')
        evidence = Path(location)
        if not (evidence / 'x' / pin['member']).is_file():
            self.skipTest('Official local Windows DLL evidence not present')
        self.assertEqual(hashlib.sha256((evidence / 'libusb-1.0.27.7z').read_bytes()).hexdigest(), pin['asset_sha256'])
        dll = (evidence / 'x' / pin['member']).read_bytes()
        self.assertEqual(hashlib.sha256(dll).hexdigest(), pin['dll_sha256'])
        self.assertEqual(len(dll), pin['dll_size'])
        with tempfile.TemporaryDirectory() as work:
            result = self.run_spec(work, dll_bytes=dll, pin=pin)
            self.assertEqual(len(result['binaries']), 1)

    def test_spec_namespace_rejects_globals_absent_from_pyinstaller(self):
        for name in ('WORKPATH', 'CONF', '__file__', '__name__', '__package__',
                     '__spec__', '__loader__', '__cached__'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as work, \
                    self.assertRaisesRegex(KeyError, re.escape(name)):
                self.run_spec(work, spec_source=f'globals()["__builtins__"]\nglobals()[{name!r}]')

    def test_spec_refuses_placeholder_licence_before_freezing(self):
        marker = '[VERIFY] PLACEHOLDER ONLY - NOT THE PYINSTALLER LICENCE TEXT.'
        with tempfile.TemporaryDirectory() as work:
            hook = Path(work) / 'diskos_build_capabilities.py'
            hook.write_text('previous hook\n', encoding='ascii')
            for args in ((), ('--enable-windows-flash', '--source-commit', 'a' * 40)):
                for text in (marker + '\n', 'padding\n' * 1000 + marker + '\n'):
                    with self.subTest(args=args, length=len(text)), \
                            self.assertRaisesRegex(SystemExit, 'PyInstaller-COPYING.*placeholder'):
                        self.run_spec(work, args, licence_text=text)
                    self.assertEqual(hook.read_text(encoding='ascii'), 'previous hook\n')
            # A short non-placeholder text passes: this is a marker check, not a length check.
            self.run_spec(work, licence_text='licence\n')
            self.run_spec(work, ('--enable-windows-flash', '--source-commit', 'a' * 40),
                          licence_text='licence\n')

    def test_windows_spec_checks_aes_and_collects_crypto(self):
        with tempfile.TemporaryDirectory() as work:
            self.run_spec(work)

    def test_spec_omits_crypto_selftest_on_every_platform(self):
        for host in (('Windows', 'AMD64'), ('Linux', 'x86_64'), ('Darwin', 'arm64')):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as work:
                result = self.run_spec(work, host=host)
                self.assertEqual(result['hiddenimports'], [
                    'usb', 'usb.core', 'usb.backend.libusb1',
                    'Crypto', 'Crypto.Cipher.AES', 'Crypto.Cipher._raw_aes', 'Crypto.Util._raw_api',
                    'Crypto.SelfTestOther',
                ])
                self.assertEqual(result['excludes'], [])

    def test_windows_entry_names_match_recovery(self):
        root = Path(__file__).resolve().parents[1]
        recovery = (root / 'RECOVERY.txt').read_text(encoding='ascii')
        self.assertIn('diskos-installer.exe', recovery)
        self.assertIn('diskos-installer-cli.exe restore-stock --firmware', recovery)
        with tempfile.TemporaryDirectory() as work:
            self.run_spec(work)  # Checks each EXE's name, entry script and retained hooks.

    def test_spec_inventory_is_complete_and_exact_on_every_platform(self):
        from test_windows_release_inventory import NATIVE, PAYLOAD
        root = Path(__file__).resolve().parents[1]
        recipe = (root / 'build/build-windows-tools.sh').read_text()
        inputs = re.search(r'\(cd "\$root" && sha256sum (.*?)\)', recipe, re.S).group(1)
        inputs = inputs.replace('\\\n', '').split()
        notices = [str(p.relative_to(root)) for p in (root / 'licenses').rglob('*')
                   if p.is_file() and p.name != 'Adobe-Glyph-List-NOTICE.txt'
                   and 'macos' not in p.relative_to(root / 'licenses').parts]  # macOS-runtime notices ship only in the Mac app
        blobs = {'my_write6_dram.bin', 'disc_spl_lpddr3.bin'}
        for host, tag in ((('Windows', 'AMD64'), 'windows-x86_64'),
                          (('Linux', 'x86_64'), 'linux-x86_64'),
                          (('Darwin', 'arm64'), 'macos-arm64')):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as work:
                result = self.run_spec(work, host=host, libfiles=['adjacent.dylib', 'scratch.dll'])
                files = {str(Path(source).relative_to(root)): dest for source, dest in result['datas']}
                expected = {'payload/' + n: 'payload' for n in PAYLOAD}
                native = NATIVE | blobs | {'MANIFEST.txt'} if host[0] == 'Windows' else blobs | {
                    'usbboot', 'mksquashfs', 'unsquashfs'}
                if host[0] == 'Darwin':
                    native = blobs  # Host Mach-O files are BINARY inputs for dependency rewriting.
                expected.update({f'vendor/{tag}/{n}': f'vendor/{tag}' for n in native})
                expected.update({f'diskos_installer/assets/{n}': 'diskos_installer/assets' for n in (
                    'wordmark.png', 'wordmark@2x.png', 'display.png', 'display@1.5x.png', 'display@2x.png',
                    'display.json', 'disc.png', 'disc@1.5x.png', 'disc@2x.png',
                    'LICENSE-archivo-OFL.txt', 'LICENSE-michroma-OFL.txt')})
                expected['site-packages/certifi/cacert.pem'] = 'certifi'
                if host[0] == 'Windows':
                    sidecars = inputs + notices + ['LICENSE', 'NOTICE.md', 'README.md',
                        'build/windows-libusb.json',
                        'corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2.asc',
                        'corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst',
                        'corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst.sig']
                    expected.update({n: os.path.dirname(n) or '.' for n in sidecars})
                elif host[0] == 'Linux':
                    expected.update({f'vendor/{tag}/lib/{n}': f'vendor/{tag}/lib'
                                     for n in ('adjacent.dylib', 'scratch.dll')})
                self.assertEqual(files, expected)
                self.assertEqual(len(files), len(result['datas']), 'Duplicate bundled file')

    def test_spec_windows_tk_pruning_and_final_collection_gate(self):
        kept = [(name, 'input', 'DATA') for name in (
            '_tk_data/tk.tcl', '_tk_data/icons.tcl', '_tk_data/ttk/ttk.tcl',
            '_tk_data/tclIndex', '_tk_data/msgbox.tcl', '_tcl_data/init.tcl')]
        removed = [(name, 'input', 'DATA') for name in (
            '_tk_data/mkpsenc.tcl', '_tk_data/images/logo.eps',
            '_tk_data/images/pwrdLogo.eps', r'_tk_data\images\logo100.gif')]
        with tempfile.TemporaryDirectory() as work:
            self.run_spec(work, analysis_datas=kept + removed)
            for entry in removed:
                with self.subTest(dest=entry[0]), self.assertRaisesRegex(
                        SystemExit, 'unused Tk data still collected'):
                    self.run_spec(work, analysis_datas=kept, tk_collected=[entry])
            with self.assertRaisesRegex(SystemExit, 'unused Tk data still collected'):
                self.run_spec(work, collection_binaries=[(removed[0][0], 'input', 'BINARY')])

    def test_spec_refuses_missing_runtime_inventory(self):
        cases = [(('Windows', 'AMD64'), name) for name in (
            'vendor/windows-x86_64/bind_winusb.exe',
            'src/windows-driver/bind_winusb.c', 'src/windows-driver/bind_winusb.rc',
            'src/windows-driver/bind_winusb.manifest', 'build/build-windows-driver.sh',
            'corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst.sig',
            'licenses/windows/msys2-runtime-BSD-NOTICES.txt',
            'licenses/FTL-FreeType.txt', 'licenses/BSL-1.0-dlg.txt')]
        cases += [(('Darwin', 'arm64'), 'vendor/macos-arm64/lib/libusb-1.0.0.dylib')]
        cases += [(host, 'payload/diskos-root.pub.pem') for host in (
            ('Windows', 'AMD64'), ('Linux', 'x86_64'), ('Darwin', 'arm64'))]
        for host, name in cases:
            with self.subTest(host=host, missing=name), tempfile.TemporaryDirectory() as work:
                diagnostic = Path(name).name if name.startswith('vendor/') else name
                with self.assertRaisesRegex(SystemExit, re.escape(diagnostic)):
                    self.run_spec(work, host=host, missing=(name,))

    def test_spec_embeds_capability_and_rewrites_default_after_enabled(self):
        with tempfile.TemporaryDirectory() as work:
            for args, enabled in (((), False), (('--enable-windows-flash', '--source-commit', 'a' * 40), True),
                                  ((), False)):
                result = self.run_spec(work, args)
                hook, = result['runtime_hooks']
                self.assertEqual(Path(hook).parent, Path(work))
                self.assertNotIn(hook, [source for source, dest in result['datas']])
                with mock.patch('diskos_installer._build_capabilities.WINDOWS_FLASH', None):
                    state = runpy.run_path(hook)['_build_capabilities']
                    self.assertIs(state.WINDOWS_FLASH, enabled)
                    self.assertEqual(state.SOURCE_COMMIT, 'a' * 40 if enabled else '')

    def test_spec_refuses_invalid_enable_requests(self):
        with tempfile.TemporaryDirectory() as work:
            for commit, version, native in (('', '1.2.3', True), ('short', '1.2.3', True),
                    ('g' * 40, '1.2.3', True), ('a' * 40, '1.2.0', True), ('a' * 40, '1.2.3', False)):
                with self.subTest(commit=commit, version=version, native=native), self.assertRaises(SystemExit):
                    self.run_spec(work, ('--enable-windows-flash', '--source-commit', commit), version, native)

    def test_spec_refuses_unverified_repository_before_rewriting_hook(self):
        top = ('rev-parse', '--show-toplevel')
        head = ('rev-parse', '--verify', 'HEAD^{commit}')
        tracked = ('status', '--porcelain', '--untracked-files=no')
        untracked = ('ls-files', '--others', '--exclude-standard', '-z', '--',
                     'diskos_installer', 'payload', 'vendor/windows-x86_64', 'src/usbboot')
        failed = subprocess.CalledProcessError(128, 'git')
        cases = [
            ('fabricated SHA', {head: 'b' * 40 + '\n'}, 'must equal installer HEAD'),
            ('dirty tracked', {tracked: ' M diskos_installer/flasher.py\n'}, 'tracked changes'),
            ('staged change', {tracked: 'M  payload/mq_ui\n'}, 'tracked changes'),
            ('no repo', {top: failed}, 'cannot verify'),
            ('ancestor repo', {top: str(Path(__file__).resolve().parents[2])}, 'repository root'),
            ('no HEAD', {head: failed}, 'cannot verify'),
            ('no git', {top: FileNotFoundError('git')}, 'cannot verify'),
            ('status failed', {tracked: failed}, 'cannot verify'),
            ('untracked check failed', {untracked: failed}, 'cannot verify'),
        ]
        for path in ('diskos_installer/extra.py', 'payload/extra',
                     'vendor/windows-x86_64/lib/extra.dll', 'src/usbboot/extra.c'):
            cases.append((path, {untracked: path + '\0'}, 'untracked files in packaged paths'))
        with tempfile.TemporaryDirectory() as work:
            hook = Path(work) / 'diskos_build_capabilities.py'
            hook.write_text('previous hook\n', encoding='ascii')
            for name, outputs, message in cases:
                with self.subTest(case=name), self.assertRaisesRegex(SystemExit, message):
                    self.run_spec(work, ('--enable-windows-flash', '--source-commit', 'a' * 40),
                                  git_outputs=outputs)
                self.assertEqual(hook.read_text(encoding='ascii'), 'previous hook\n')
                self.assertEqual(list(Path(work).iterdir()), [hook])

    def test_default_spec_needs_neither_repository_nor_git(self):
        with tempfile.TemporaryDirectory() as work:
            self.run_spec(work, git_outputs={('rev-parse', '--show-toplevel'): FileNotFoundError('git')})
