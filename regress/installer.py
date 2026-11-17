#!/usr/bin/env python3
"""Exercise the piped installer without changing the host's package state.

Platform, network, package-manager and signature-tool commands are mocked;
the installer itself runs under /bin/sh with stdin supplied as a curl pipe.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FINGERPRINT = "B7B61E8CB85A9270530364F23273CB62402FC793"

MOCK = r'''
import json, os, pathlib, shlex, shutil, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
env = os.environ
with open(env['ODIN_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name, *args]) + '\n')
if env.get('ODIN_TEST_FAIL_COMMAND') == ' '.join([name, *args[:1]]):
    sys.exit(42)
if name == 'uname':
    print(env.get('ODIN_TEST_OS', 'Linux'))
elif name == 'id':
    print(env.get('ODIN_TEST_UID', '1000'))
elif name == 'dpkg':
    print(env.get('ODIN_TEST_ARCH', 'amd64'))
elif name == 'curl':
    url = next(arg for arg in args if arg.startswith('https://'))
    assert '--proto' in args and args[args.index('--proto') + 1] == '=https'
    assert '--tlsv1.2' in args
    if env.get('ODIN_TEST_FAIL_DOWNLOAD', '!') in url:
        sys.exit(22)
    output = pathlib.Path(args[args.index('-o') + 1])
    if url.endswith('/InRelease'):
        output.write_text('Codename: ' + env.get('ODIN_TEST_RELEASE_SUITE', 'resolute') + '\nArchitectures: ' + env.get('ODIN_TEST_ARCHITECTURES', 'amd64 arm64') + '\n')
    elif url.endswith('/keyring.gpg') or url.endswith('/goblinreactor-archive-keyring.gpg'):
        output.write_text('fixture archive key')
    elif url == 'https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh':
        brew = pathlib.Path(env['ODIN_TEST_PREFIX']) / 'bin/brew'
        output.write_text('mkdir -p ' + shlex.quote(str(brew.parent)) + '\nln -sf ' + shlex.quote(env['ODIN_TEST_MOCK']) + ' ' + shlex.quote(str(brew)) + '\n')
    else:
        raise AssertionError(url)
elif name == 'gpg':
    assert '--no-options' in args and '--homedir' in args
    if '--decrypt' in args:
        assert '--no-default-keyring' in args and '--keyring' in args
        shutil.copyfile(args[-1], args[args.index('--output') + 1])
    else:
        for _ in range(int(env.get('ODIN_TEST_KEY_COUNT', '1'))):
            print('pub::::::::::')
            print('fpr:::::::::' + env.get('ODIN_TEST_FINGERPRINT', 'B7B61E8CB85A9270530364F23273CB62402FC793') + ':')
elif name == 'gpgv':
    assert '--keyring' in args and '--homedir' in args
    if env.get('ODIN_TEST_BAD_SIGNATURE'):
        sys.exit(1)
elif name == 'sudo':
    sys.exit(subprocess.call(args))
elif name == 'install':
    sys.exit(subprocess.call([env['ODIN_TEST_REAL_INSTALL'], *args]))
elif name in ('brew', 'apt-get'):
    # A child reading stdin must never consume the rest of the installer.
    assert sys.stdin.read() == ''
    if name == 'brew' and args == ['--prefix']:
        print(env['ODIN_TEST_PREFIX'])
    if (name == 'brew' and args[:1] == ['install']) or (name == 'apt-get' and 'odin-seat' in args):
        binary = pathlib.Path(env['ODIN_TEST_BINARY']) if name == 'apt-get' else pathlib.Path(env['ODIN_TEST_PREFIX']) / 'bin/odin-seat'
        binary.parent.mkdir(parents=True, exist_ok=True)
        if not binary.exists():
            binary.symlink_to(env['ODIN_TEST_MOCK'])
elif name == 'odin-seat':
    assert args == ['-V']
    print('Odin Seat fixture version')
else:
    raise AssertionError(name)
'''


class Installer(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="odin-seat-installer-test-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.tmp = self.work / "tmp"
        self.tmp.mkdir()
        self.log = self.work / "commands.jsonl"
        self.mock = self.work / "mock.py"
        self.mock.write_text("#!" + sys.executable + "\n" + MOCK)
        self.mock.chmod(0o755)
        for name in ("uname", "id", "dpkg", "curl", "gpg", "gpgv", "sudo", "apt-get", "brew", "install"):
            (self.bin / name).symlink_to(self.mock)
        self.os_release = self.work / "os-release"
        self.os_release.write_text('ID=ubuntu\nVERSION_CODENAME=resolute\n')
        self.sources = self.work / "etc/apt/sources.list.d/goblinreactor.sources"
        self.prefix = self.work / "homebrew"
        self.binary = self.work / "usr/bin/odin-seat"
        self.script = (ROOT / "html/install.sh").read_text()
        # Redirect the installer's system paths into this disposable fixture.
        for original, replacement in (
            ("/etc/os-release", self.os_release),
            ("/etc/apt/", self.work / "etc/apt/"),
            ("/opt/homebrew/bin/brew", self.prefix / "bin/brew"),
            ("/usr/local/bin/brew", self.work / "other-homebrew/bin/brew"),
            ("/usr/bin/odin-seat", self.binary),
        ):
            self.script = self.script.replace(original, str(replacement) + ("/" if original.endswith("/") else ""))
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("ODIN_TEST_")}
        self.env.update(
            PATH=str(self.bin) + os.pathsep + "/usr/bin:/bin",
            TMPDIR=str(self.tmp),
            ODIN_TEST_LOG=str(self.log),
            ODIN_TEST_MOCK=str(self.mock),
            ODIN_TEST_PREFIX=str(self.prefix),
            ODIN_TEST_BINARY=str(self.binary),
            ODIN_TEST_REAL_INSTALL=shutil.which("install"),
        )

    def run_installer(self, success=True, script=None, args=(), **env):
        result = subprocess.run(
            ["/bin/sh", "-s", "--", *args], input=self.script if script is None else script,
            text=True, capture_output=True, env={**self.env, **env},
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("Odin Seat is installed", result.stdout)
        self.assertEqual(list(self.tmp.iterdir()), [], "Installer left temporary files behind")
        return result

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_linux_signed_repository_and_rerun(self):
        self.run_installer()
        sources = self.sources.read_text()
        self.assertIn("Suites: resolute\n", sources)
        self.assertIn("Architectures: amd64\n", sources)
        self.assertIn("Signed-By: " + str(self.work / "etc/apt/keyrings/goblinreactor-archive-keyring.gpg"), sources)
        commands = self.commands()
        self.assertTrue(any(command[:2] == ["sudo", "install"] for command in commands))
        self.assertLess(next(i for i, c in enumerate(commands) if c[0] == "gpgv"), next(i for i, c in enumerate(commands) if c[0] == "install"))
        self.run_installer()
        self.assertEqual(self.sources.read_text(), sources)

    def test_linux_root_needs_no_sudo(self):
        (self.bin / "sudo").unlink()
        self.run_installer(ODIN_TEST_UID="0")
        self.assertFalse(any(c[0] == "sudo" for c in self.commands()))

    def test_ubuntu_derivative_and_arm64(self):
        self.os_release.write_text('ID=linuxmint\nVERSION_CODENAME=mint\nUBUNTU_CODENAME=resolute\n')
        self.run_installer(ODIN_TEST_ARCH="arm64")
        self.assertIn("Suites: resolute\n", self.sources.read_text())
        self.assertIn("Architectures: arm64\n", self.sources.read_text())

    def test_published_future_platform_has_no_hardcoded_limit(self):
        self.os_release.write_text('ID=debian\nVERSION_CODENAME=future\n')
        self.run_installer(ODIN_TEST_RELEASE_SUITE="future", ODIN_TEST_ARCH="riscv64", ODIN_TEST_ARCHITECTURES="riscv64")

    def test_missing_codename(self):
        self.os_release.write_text('ID=debian\n')
        self.run_installer(success=False)
        self.assertFalse(any(c[0] == "curl" for c in self.commands()))

    def test_bootstrap_verification_tools(self):
        self.script = self.script.replace(
            'if ! command -v gpg >/dev/null 2>&1 || ! command -v gpgv >/dev/null 2>&1; then',
            'if true; then',
        )
        self.run_installer()
        self.assertIn(["apt-get", "install", "-y", "ca-certificates", "gnupg"], self.commands())

    def test_unpublished_release(self):
        self.run_installer(success=False, ODIN_TEST_FAIL_DOWNLOAD="/InRelease")
        self.assertFalse(self.sources.exists())
        self.assertFalse(any(c[0] == "apt-get" for c in self.commands()))

    def test_wrong_key(self):
        self.run_installer(success=False, ODIN_TEST_FINGERPRINT="0" * len(FINGERPRINT))
        self.assertFalse(self.sources.exists())
        self.assertFalse(any(c[0] == "gpgv" for c in self.commands()))

    def test_extra_primary_key(self):
        self.run_installer(success=False, ODIN_TEST_KEY_COUNT="2")
        self.assertFalse(self.sources.exists())

    def test_bad_signature(self):
        self.run_installer(success=False, ODIN_TEST_BAD_SIGNATURE="1")
        self.assertFalse(self.sources.exists())

    def test_wrong_signed_codename(self):
        self.run_installer(success=False, ODIN_TEST_RELEASE_SUITE="other")
        self.assertFalse(self.sources.exists())

    def test_unpublished_architecture(self):
        self.run_installer(success=False, ODIN_TEST_ARCH="riscv64")
        self.assertFalse(self.sources.exists())

    def test_package_failure_propagates(self):
        self.run_installer(success=False, ODIN_TEST_FAIL_COMMAND="apt-get install")

    def test_macos_existing_homebrew(self):
        self.run_installer(ODIN_TEST_OS="Darwin")
        self.assertIn(["brew", "tap", "goblinreactor/tap", "https://brew.goblinreactor.com/homebrew-tap.git"], self.commands())
        self.assertIn(["brew", "install", "goblinreactor/tap/odin-seat"], self.commands())

    def test_macos_bootstraps_homebrew(self):
        (self.bin / "brew").unlink()
        self.run_installer(ODIN_TEST_OS="Darwin")
        self.assertTrue((self.prefix / "bin/brew").exists())

    def test_homebrew_download_failure(self):
        (self.bin / "brew").unlink()
        self.run_installer(success=False, ODIN_TEST_OS="Darwin", ODIN_TEST_FAIL_DOWNLOAD="/Homebrew/install/")

    def test_homebrew_failure_propagates(self):
        self.run_installer(success=False, ODIN_TEST_OS="Darwin", ODIN_TEST_FAIL_COMMAND="brew install")

    def test_macos_root_is_refused(self):
        self.run_installer(success=False, ODIN_TEST_OS="Darwin", ODIN_TEST_UID="0")
        self.assertFalse(any(c[0] == "brew" for c in self.commands()))

    def test_unsupported_os(self):
        self.run_installer(success=False, ODIN_TEST_OS="FreeBSD")

    def test_non_apt_linux(self):
        (self.bin / "apt-get").unlink()
        # Hide any host apt-get as well, including on a Linux test host.
        self.script = self.script.replace("command -v apt-get", "command -v odin-test-missing-apt")
        self.run_installer(success=False)
        self.assertFalse(self.sources.exists())

    def test_help_and_invalid_option(self):
        self.run_installer(args=("--help",))
        self.assertEqual(self.commands(), [])
        self.run_installer(success=False, args=("--unknown",))

    def test_incomplete_download_does_not_start_installing(self):
        self.run_installer(script=self.script.rsplit('main "$@"', 1)[0])
        self.assertEqual(self.commands(), [])


if __name__ == "__main__":
    unittest.main()
