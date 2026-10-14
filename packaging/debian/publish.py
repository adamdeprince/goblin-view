#!/usr/bin/env python3
"""Publish goblin-view into the Goblin Reactor APT repository on hail.

Runs on hail as the repository administrator. Reads the per-suite build
artifacts staged under STAGE/artifacts/<suite>/ (what build-all.sh left in
artifacts/), adds each .dsc and .deb to its release-named suite with the
private reprepro configuration, re-exports and verifies the signed indexes,
publishes the build logs, and then (with --docs, once the public-install
tests have passed) rewrites the catalog and the documentation pages so they
list every package the repository actually holds.

    publish.py packages     # reprepro: include, export, verify, copy logs
    publish.py docs         # packages.json (signed), README.md/.txt, index.html

Nothing already in the pool is touched: reprepro refuses a changed file
under an existing name, and every earlier catalog artifact is re-checked
against its recorded checksum before the new catalog is signed.
"""
import hashlib
import html
import json
import os
import pathlib
import re
import subprocess
import sys
from urllib.parse import urlparse

RELEASE = '20260910'
PACKAGE = 'goblin-view'
VERSION_BASE = '0.1.0+20260910.1'
SUITES = ['jammy', 'noble', 'resolute', 'bookworm', 'trixie']
DISTRIBUTION = {'jammy': 'Ubuntu 22.04 LTS', 'noble': 'Ubuntu 24.04 LTS',
                'resolute': 'Ubuntu 26.04 LTS', 'bookworm': 'Debian 12',
                'trixie': 'Debian 13'}
STAGE = pathlib.Path.home() / '.local/share/goblin-apt' / f'{PACKAGE}-release-{RELEASE}'
ADMIN = pathlib.Path.home() / '.local/share/goblin-apt/reprepro'
KEYHOME = str(pathlib.Path.home() / '.local/share/goblin-apt/gnupg')
PUBLIC = pathlib.Path('/mnt/distribution/apt.goblinreactor.com')
KEY = 'B7B61E8CB85A9270530364F23273CB62402FC793'
BASE = 'https://apt.goblinreactor.com/'
LOGS_DIR = f'releases/{RELEASE}/{PACKAGE}'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, **kw):
    print('+', ' '.join(args))
    return subprocess.run(args, check=True, **kw)


def atomic_copy(source, target, mode=0o644):
    target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    temporary = target.with_name(target.name + f'.{PACKAGE}-{RELEASE}.tmp')
    assert not temporary.exists(), temporary
    with temporary.open('xb') as stream:
        stream.write(source.read_bytes())
    temporary.chmod(mode)
    os.replace(temporary, target)


def version(suite):
    return f'{VERSION_BASE}+{suite}1'


def artifacts_dir(suite):
    return STAGE / 'artifacts' / suite


def check_stage():
    for suite in SUITES:
        d = artifacts_dir(suite)
        deb = d / f'{PACKAGE}_{version(suite)}_amd64.deb'
        dsc = d / f'{PACKAGE}_{version(suite)}.dsc'
        assert deb.is_file() and dsc.is_file(), (deb, dsc)
        log = (d / 'build.log').read_text(errors='replace')
        assert f'Package built, installed and exercised: {version(suite)}' in log, suite
        assert 'regress/e2e.py: ok' in log and 'regress/skeleton.sh: ok' in log, suite
        for name in ('test-desktop', 'test-host', 'test-menu', 'test-term', 'test-im', 'test-codec'):
            assert f'{name}: ok' in log, (suite, name)
        info = subprocess.check_output(['dpkg-deb', '-f', str(deb), 'Package', 'Version', 'Architecture'], text=True)
        assert info.split() == ['Package:', PACKAGE, 'Version:', version(suite), 'Architecture:', 'amd64'], info
    print('PASS: five staged builds, each with its tests passed and a matching .dsc')


def check_old_catalog(catalog):
    """Every artifact the public catalog lists is still exactly what it says."""
    for package in catalog['packages']:
        for artifact in package['artifacts']:
            url = urlparse(artifact['url'])
            assert url.scheme == 'https' and url.netloc == 'apt.goblinreactor.com'
            assert '..' not in pathlib.PurePosixPath(url.path).parts
            file = PUBLIC / url.path.lstrip('/')
            assert file.stat().st_size == artifact['size'], file
            assert sha256(file) == artifact['sha256'], file


def publish_packages():
    check_stage()
    backup = STAGE / 'backup'
    assert not backup.exists(), 'a backup exists: this step already ran'
    backup.mkdir(mode=0o700)
    run('cp', '-a', str(ADMIN / 'conf'), str(backup / 'conf'))
    run('cp', '-a', str(ADMIN / 'db'), str(backup / 'db'))
    for name in ('packages.json', 'packages.json.asc', 'README.md', 'README.txt', 'index.html'):
        run('cp', '-a', str(PUBLIC / name), str(backup / name))
    conf = (ADMIN / 'conf/distributions').read_text()
    for suite in SUITES:
        assert f'\nCodename: {suite}\n' in conf, suite
    for suite in SUITES:
        d = artifacts_dir(suite)
        for name in (f'{PACKAGE}_{version(suite)}.dsc', f'{PACKAGE}_{version(suite)}_amd64.deb'):
            assert not (PUBLIC / 'pool/main/g' / PACKAGE / name).exists(), name
        run('reprepro', '-b', str(ADMIN), '--export=never', 'includedsc', suite,
            str(d / f'{PACKAGE}_{version(suite)}.dsc'))
        run('reprepro', '-b', str(ADMIN), '--export=never', 'includedeb', suite,
            str(d / f'{PACKAGE}_{version(suite)}_amd64.deb'))
    run('reprepro', '-b', str(ADMIN), 'check', *SUITES)
    run('reprepro', '-b', str(ADMIN), 'export', *SUITES)
    for suite in SUITES:
        run('gpgv', '--keyring', str(PUBLIC / 'goblinreactor-archive-keyring.gpg'),
            str(PUBLIC / 'dists' / suite / 'InRelease'))
        listing = subprocess.check_output(['reprepro', '-b', str(ADMIN), 'list', suite], text=True)
        assert f'{PACKAGE} {version(suite)}' in listing, listing
        print(listing.strip())
        for file in sorted(artifacts_dir(suite).iterdir()):
            if file.suffix in {'.log', '.txt', '.buildinfo', '.changes'}:
                atomic_copy(file, PUBLIC / LOGS_DIR / suite / file.name)
    print('PASS: goblin-view added to five signed suites; run the public-install tests, then `docs`')


# ---------------------------------------------------------------- docs ----

def package_entries():
    entries = []
    for suite in SUITES:
        d = artifacts_dir(suite)
        deb_name = f'{PACKAGE}_{version(suite)}_amd64.deb'
        pool = pathlib.Path('pool/main/g') / PACKAGE
        install_log = d / 'public-install.log'
        assert install_log.is_file(), f'no public-install.log for {suite}: run the install test first'
        assert f'PASS: public signed APT install and smoke test: {suite}' in install_log.read_text(), suite
        entry = {
            'name': PACKAGE, 'format': 'deb', 'distribution': DISTRIBUTION[suite],
            'suite': suite, 'architecture': 'amd64', 'version': version(suite),
            'depends': subprocess.check_output(['dpkg-deb', '-f', str(PUBLIC / pool / deb_name), 'Depends'],
                                               text=True).strip(),
            'base_image': (d / 'base-image-digest.txt').read_text().split()[1] + '@' +
                          (d / 'base-image-digest.txt').read_text().split()[0],
            'verification': {'unit_tests': 'pass', 'end_to_end': 'pass', 'installed_smoke': 'pass',
                             'public_install': 'pass'},
            'artifacts': [],
        }
        for name in (f'{PACKAGE}_{version(suite)}.dsc', f'{PACKAGE}_{version(suite)}.tar.xz', deb_name):
            file = PUBLIC / pool / name
            entry['artifacts'].append({'filename': name, 'url': BASE + str(pool / name),
                                       'size': file.stat().st_size, 'sha256': sha256(file)})
        for file in sorted(d.iterdir()):
            if file.suffix not in {'.log', '.txt', '.buildinfo', '.changes'}:
                continue
            relative = pathlib.Path(LOGS_DIR) / suite / file.name
            atomic_copy(file, PUBLIC / relative)
            entry['artifacts'].append({'filename': file.name, 'url': BASE + str(relative),
                                       'size': file.stat().st_size, 'sha256': sha256(file)})
        entry['artifacts'].sort(key=lambda a: a['filename'])
        entries.append(entry)
    return entries


def render_markdown(md):
    """The subset of Markdown these pages use, as the existing page's HTML."""
    out, i, lines = [], 0, md.splitlines()

    def inline(text):
        text = html.escape(text, quote=False)
        text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', text)
        text = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', text)
        text = re.sub(r'`([^`]+)`', r'<code>\1</code>', text)
        return text

    def slug(text):
        return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')

    while i < len(lines):
        line = lines[i]
        if line.startswith('# '):
            out.append(f'<h1 id="{slug(line[2:])}">{inline(line[2:])}</h1>')
            i += 1
        elif line.startswith('## '):
            out.append(f'<h2 id="{slug(line[3:])}">{inline(line[3:])}</h2>')
            i += 1
        elif line.startswith('### '):
            out.append(f'<h3 id="{slug(line[4:])}">{inline(line[4:])}</h3>')
            i += 1
        elif line.startswith('~~~'):
            i += 1
            block = []
            while i < len(lines) and not lines[i].startswith('~~~'):
                block.append(lines[i])
                i += 1
            i += 1
            out.append('<pre><code>' + html.escape('\n'.join(block)) + '</code></pre>')
        elif line.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].startswith('|'):
                rows.append([c.strip() for c in lines[i].strip().strip('|').split('|')])
                i += 1
            head, body = rows[0], rows[2:]
            th = ''.join(f'<th scope="col">{inline(c)}</th>' for c in head)
            trs = ''.join('<tr>' + ''.join(f'<td>{inline(c)}</td>' for c in r) + '</tr>' for r in body)
            out.append(f'<div class="table-wrap"><table><thead><tr>{th}</tr></thead><tbody>{trs}</tbody></table></div>')
        elif line.strip() == '':
            i += 1
        else:
            para = [line.strip()]		# always consume this line
            i += 1
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(('#', '~~~', '|')):
                para.append(lines[i].strip())
                i += 1
            out.append('<p>' + inline(' '.join(para)) + '</p>')
    return '\n'.join(out)


HTML_HEAD = '''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Goblin Reactor package repository</title><meta name="description" content="Signed Goblin Reactor packages for Debian, Ubuntu, Fedora and Rocky Linux: goblin-mosh and goblin-view, matching source, checksums and installation instructions."><style>
:root{color-scheme:light dark;--bg:#f7f9f4;--fg:#192b21;--accent:#246f48;--panel:#e9eee5;--line:#b7c6b7}
@media(prefers-color-scheme:dark){:root{--bg:#111912;--fg:#e3eedf;--accent:#a4d79a;--panel:#1b281e;--line:#465b46}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:17px/1.6 system-ui,sans-serif}
main{max-width:1250px;margin:auto;padding:2rem 1.25rem 4rem}h1{font-size:clamp(1.8rem,4vw,2.5rem);line-height:1.2}
h2{margin-top:2.4rem;padding-top:1rem;border-top:1px solid var(--line);font-size:1.3rem}h3{margin-top:1.8rem;font-size:1.1rem}
a{color:var(--accent);text-underline-offset:.2em}a:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
nav{display:flex;gap:1rem;flex-wrap:wrap;font-size:.95rem}pre{overflow:auto;padding:1.15rem;background:var(--panel);border:1px solid var(--line);border-radius:7px}
code{font:14px/1.65 ui-monospace,SFMono-Regular,Consolas,monospace}.table-wrap{overflow:auto}
table{border-collapse:collapse;width:100%;font-size:.85rem}th,td{text-align:left;padding:.6rem;border-bottom:1px solid var(--line)}
th{background:var(--panel)}p{max-width:85ch;overflow-wrap:anywhere}footer{margin-top:3rem;font-size:.9rem;border-top:1px solid var(--line);padding-top:1rem}
</style></head><body><main><nav aria-label="Repository"><a href="https://apt.goblinreactor.com/#available-packages">Packages</a><a href="https://apt.goblinreactor.com/#set-up-apt-on-debian-or-ubuntu">APT setup</a><a href="https://apt.goblinreactor.com/rpm/#set-up-dnf">RPM setup</a><a href="README.txt">Plain text</a><a href="https://apt.goblinreactor.com/packages.json">Checksummed catalog</a><a href="https://apt.goblinreactor.com/packages.json.asc">Catalog signature</a></nav>
'''
HTML_FOOT = '''<footer>Goblin Reactor · Updated {date} · <a href="https://apt.goblinreactor.com/goblinreactor-archive-keyring.asc">Public signing key</a></footer></main></body></html>
'''


def deb_table(catalog, name):
    rows = ['| Distribution | APT suite | Architecture | Package version | Binary | Matching source |',
            '| --- | --- | --- | --- | --- | --- |']
    order = {s: n for n, s in enumerate(SUITES)}
    packages = sorted((p for p in catalog['packages'] if p['name'] == name and p['format'] == 'deb'),
                      key=lambda p: (order.get(p['suite'], 99), p['architecture']))
    for p in packages:
        by = {a['filename']: a for a in p['artifacts']}
        deb = next(a for f, a in by.items() if f.endswith('.deb'))
        dsc = next(a for f, a in by.items() if f.endswith('.dsc'))
        tar = next(a for f, a in by.items() if f.endswith('.tar.xz'))
        rows.append(f"| {p['distribution']} | {p['suite']} | {p['architecture']} | {p['version']} | "
                    f"[.deb]({deb['url']}) | [.dsc]({dsc['url']}) · [source archive]({tar['url']}) |")
    return '\n'.join(rows)


def rpm_table(catalog):
    rows = ['| Distribution | Architecture | Package version | Binary | Matching source |',
            '| --- | --- | --- | --- | --- |']
    for p in (p for p in catalog['packages'] if p['format'] == 'rpm'):
        by = {a['filename']: a for a in p['artifacts']}
        rpm = next(a for f, a in by.items() if f.endswith('.rpm') and not f.endswith('.src.rpm'))
        src = next(a for f, a in by.items() if f.endswith('.src.rpm'))
        rows.append(f"| {p['distribution']} | {p['architecture']} | {p['version']} | "
                    f"[.rpm]({rpm['url']}) | [.src.rpm]({src['url']}) |")
    return '\n'.join(rows)


def render_readme(catalog, date):
    names = sorted({p['name'] for p in catalog['packages'] if p['format'] == 'deb'})
    md = []
    md.append('# Goblin Reactor package repository\n')
    md.append('Third-party Debian, Ubuntu, Fedora and Rocky Linux packages maintained by Adam DePrince. '
              'Public repository: https://apt.goblinreactor.com/\n')
    md.append('## Available packages\n')
    md.append('Two binary packages are published for Debian and Ubuntu: **goblin-mosh** and '
              '**goblin-view**. Each is built separately against every supported release; use the '
              'suite matching your OS, not a different release\'s package. The older empty "stable" '
              'suite is not a universal Debian/Ubuntu binary target; use the release-specific suites '
              'below. Everything listed here has been installed and exercised through the signed '
              'public repository before being listed.\n')
    md.append('### goblin-mosh\n')
    md.append('Goblin Mosh extends Mosh for bandwidth-constrained links: zstd level 22, independent '
              'upload/download pacing, idle keepalive backoff, terminal graphics, and a two-pane '
              'file-transfer menu with librsync and FEC. The package includes the client, server, '
              'goblin-moshcp, dictionary compiler, man pages and Bash completion; these are bundled '
              'tools, not separate APT packages.\n')
    md.append(deb_table(catalog, 'goblin-mosh') + '\n')
    md.append('### goblin-view\n')
    md.append('GoblinView is a terminal multiplexer with overlapping windows and an Alt-tap menu, not '
              'tiled panes. Each window is a terminal of its own kept in a server process, so a '
              'session survives detaching; windows are moved, resized, selected and scrolled with the '
              'mouse; images drawn with kitty graphics or sixel are re-emitted in whatever the terminal '
              'you attach from speaks; and programs in windows get the kitty protocol extensions per '
              'window (keyboard protocol, styled underlines, colours, pointer shapes, extra cursors, '
              'drag and drop, file transfer). It runs best in Ghostty or kitty and works anywhere a '
              'terminal does. The package installs `/usr/bin/goblin-view`, the manual pages '
              '`goblin-view(1)` and `goblin-view-protocols(7)`, and the input-method dictionaries.\n')
    md.append(deb_table(catalog, 'goblin-view') + '\n')
    md.append('ARM64 and 32-bit builds are not published for either package. Only goblin-mosh is '
              'available as RPMs.\n')
    md.append('## RPM packages\n')
    md.append('The goblin-mosh tools are also available as native RPMs:\n')
    md.append(rpm_table(catalog) + '\n')
    md.append('[RPM installation instructions](https://apt.goblinreactor.com/rpm/) explain DNF setup, '
              'EPEL/CRB dependencies on Rocky, signature verification and source RPMs. Use the build for '
              'your distribution and release; other RPM distributions have not been tested. Binary RPMs, '
              'SRPMs and repository metadata are signed with the same archive key used by APT. No RPM or '
              'Homebrew package of goblin-view is published here.\n')
    md.append('## Set up APT on Debian or Ubuntu\n')
    md.append('This installs a dedicated repository key and a deb822 source file. It does not replace '
              'your distribution\'s repositories or disable signature checking. If you previously added '
              'the old "stable" entry, remove that old entry from its .list/.sources file to avoid '
              'keeping an unused source. The one setup serves every package here.\n')
    md.append('First install the setup tools:\n')
    md.append('~~~sh\nsudo apt update\nsudo apt install ca-certificates curl gnupg\n~~~\n')
    md.append('Then run this block. It refuses unsupported releases and architectures:\n')
    md.append('''~~~sh
(
    set -eu
    . /etc/os-release
    case "$ID:$VERSION_CODENAME" in
        ubuntu:jammy|ubuntu:noble|ubuntu:resolute|debian:bookworm|debian:trixie) ;;
        *) echo "No package is published for this OS release." >&2; exit 1 ;;
    esac
    test "$(dpkg --print-architecture)" = amd64 || {
        echo "Only amd64 packages are currently published." >&2
        exit 1
    }

    goblin_keydir=$(mktemp -d)
    trap 'rm -f "$goblin_keydir/keyring.gpg"; rmdir "$goblin_keydir"' EXIT
    curl -fsSL https://apt.goblinreactor.com/goblinreactor-archive-keyring.gpg \\
        -o "$goblin_keydir/keyring.gpg"
    goblin_fingerprint=$(gpg --batch --show-keys --with-colons \\
        "$goblin_keydir/keyring.gpg" | awk -F: '$1 == "fpr" { print $10; exit }')
    test "$goblin_fingerprint" = B7B61E8CB85A9270530364F23273CB62402FC793 || {
        echo "Unexpected repository signing key; stopping." >&2
        exit 1
    }
    sudo install -d -m 0755 /etc/apt/keyrings
    sudo install -m 0644 "$goblin_keydir/keyring.gpg" \\
        /etc/apt/keyrings/goblinreactor-archive-keyring.gpg
    printf '%s\\n' \\
        'Types: deb deb-src' \\
        'URIs: https://apt.goblinreactor.com/' \\
        "Suites: $VERSION_CODENAME" \\
        'Components: main' \\
        'Architectures: amd64' \\
        'Signed-By: /etc/apt/keyrings/goblinreactor-archive-keyring.gpg' \\
        | sudo tee /etc/apt/sources.list.d/goblinreactor.sources >/dev/null
)
sudo apt update
~~~
''')
    md.append('Then install what you want:\n')
    md.append('~~~sh\napt-cache policy goblin-view goblin-mosh\nsudo apt install goblin-view\n'
              'sudo apt install goblin-mosh\n~~~\n')
    md.append('Signing-key fingerprint:\n')
    md.append('~~~text\nB7B6 1E8C B85A 9270 5303 64F2 3273 CB62 402F C793\n~~~\n')
    md.append('The key is scoped to this repository with Signed-By. Do not use apt-key, trusted=yes, '
              '--allow-unauthenticated, or disable TLS verification. This is an independent third-party '
              'archive, not an official Debian or Ubuntu archive. Installing a package grants its '
              'maintainer the trust normally required for system software.\n')
    md.append('## Use, update and remove\n')
    md.append('Every package here is updated, inspected and removed the same way:\n')
    md.append('~~~sh\nsudo apt update\nsudo apt install --only-upgrade goblin-view goblin-mosh\n'
              'dpkg-query -W goblin-view goblin-mosh\nsudo apt remove goblin-view\n~~~\n')
    md.append('After an OS release upgrade, change the suite in goblinreactor.sources to the new '
              'supported codename (or rerun setup), then update and upgrade. Do not add Debian suites to '
              'Ubuntu or Ubuntu suites to Debian.\n')
    md.append('### Using goblin-mosh\n')
    md.append('Install goblin-mosh on the client and remote server, then connect:\n')
    md.append('~~~sh\ngoblin-mosh user@server\n~~~\n')
    md.append('SSH must reach the server for login, and its firewall must allow the selected Mosh UDP '
              'port (normally 60000–61000). Installing the package does not open firewall ports or start '
              'a permanent system daemon. The package installs /usr/bin/goblin-mosh and does not replace '
              '/usr/bin/mosh. An existing distribution mosh package can stay installed. A previously '
              'packaged adam-mosh is obsolete and conflicts with goblin-mosh. The file browser opens with '
              'Mosh\'s command prefix: Ctrl-^ (often Ctrl-Shift-6), then 0. Enter starts a transfer '
              'without leaving the browser; x cancels. Files are synchronized in both directions with '
              'librsync and FEC. Open a new session after an upgrade to use the new client and server; '
              'existing processes are not restarted.\n')
    md.append('### Using goblin-view\n')
    md.append('Run `goblin-view` in a terminal to attach to the default session, creating it with one '
              'window if none exists. Tap Alt (Option on a Mac, either side) and let go to open the menu, '
              'then a letter: O opens a window, C closes, Z zooms, R rearranges, 1-9 selects, D '
              'detaches, Q quits, ? shows help. `Ctrl-\\` opens the menu on a terminal that swallows the '
              'Alt tap. `man goblin-view` covers the command, keys, mouse and environment; '
              '`man goblin-view-protocols` lists what programs in windows can use. On macOS Ghostty '
              'needs `macos-option-as-alt = true` in its configuration for the Option tap to arrive.\n')
    md.append('~~~sh\ngoblin-view\ngoblin-view new-window -- htop\ngoblin-view list-windows\n'
              'goblin-view kill-server\n~~~\n')
    md.append('A goblin-view server keeps running across package upgrades; a newer client that attaches '
              'to it upgrades it in place (`goblin-view upgrade` does the same by hand), keeping every '
              'window and attached terminal. A server too old for that is refused with a message; close '
              'its windows and run `goblin-view kill-server`. If you previously installed goblin-view by '
              'hand under ~/.local/bin or /usr/local/bin, that copy can shadow /usr/bin: check '
              '`command -v goblin-view` and remove the manual copy yourself; the package does not delete '
              'administrator-managed files.\n')
    md.append('## Matching source and licenses\n')
    md.append('Complete matching native Debian source packages are published alongside the binaries in '
              'each suite. With deb-src enabled above:\n')
    md.append('~~~sh\nmkdir -p goblin-source\ncd goblin-source\napt-get source goblin-view\n'
              'sudo apt build-dep goblin-view\ncd goblin-view-*\ndpkg-buildpackage -us -uc -b\n~~~\n')
    md.append('The same works for goblin-mosh. Each source archive includes the code, Debian packaging, '
              'and container build recipes; release-specific .buildinfo files record build dependencies. '
              'Source, binaries, and verification artifacts are linked from the catalog. RPM builds have '
              'matching signed source RPMs in their release\'s source repository.\n')
    md.append('Goblin Mosh is derived from Mosh and remains GPL-3.0-or-later, with existing notices and '
              'linking exceptions preserved. librsync, zstd, WebP, PNG, OpenSSL and other runtime '
              'libraries are dynamically linked distribution packages rather than bundled copies; their '
              'licenses remain applicable. See /usr/share/doc/goblin-mosh/copyright and THIRD_PARTY.md. '
              'The optional --fips-crypto mode is built, but no validated FIPS provider is included.\n')
    md.append('GoblinView is original code under the Apache License 2.0; its bundled input-method '
              'dictionaries are generated from MIT-, Apache-2.0- and Unicode-licensed sources, recorded '
              'in /usr/share/doc/goblin-view/copyright and /usr/share/goblin-view/README.md. It links '
              'only zlib and the C++ runtime.\n')
    md.append('## Verification and support\n')
    md.append('Packages are built separately against the listed distributions\' libraries in disposable '
              'containers from pinned base images. For goblin-mosh the release checks cover core '
              'protocol/rendering/compression/file-worker tests, installed encrypted file transfers, and '
              'installation through signed APT metadata. For goblin-view they cover the unit tests, the '
              'socket-level and end-to-end pseudo-terminal tests, a smoke test of the installed package '
              'in the build container, and installation through the signed public repository in a fresh '
              'container of each release. The logs are published under releases/ and listed in the '
              'catalog with checksums.\n')
    md.append('Goblin Mosh source branch: https://github.com/adamdeprince/mosh/tree/low-bandwidth-mosh '
              '· Issues: https://github.com/adamdeprince/mosh/issues · Project page: '
              'https://mosh.goblinreactor.com/\n')
    md.append('GoblinView has no public repository yet; its complete source is the matching source '
              'package in each suite.\n')
    md.append('APT source-file documentation: '
              'https://manpages.debian.org/bookworm/apt/sources.list.5.en.html\n')
    md.append('Published packages in the catalog are the available inventory; planned features, '
              'requested architectures and unpackaged tools are not additional APT packages.\n')
    return '\n'.join(md)


def publish_docs():
    catalog = json.loads((PUBLIC / 'packages.json').read_text())
    check_old_catalog(catalog)
    catalog['packages'] = [p for p in catalog['packages'] if p['name'] != PACKAGE]
    catalog['packages'] += package_entries()
    date = subprocess.check_output(['date', '-u', '+%Y-%m-%d'], text=True).strip()
    catalog['updated'] = date
    catalog['verified'] = subprocess.check_output(['date', '-u', '+%Y-%m-%dT%H:%M:%SZ'], text=True).strip()
    check_old_catalog(catalog)
    candidate = STAGE / 'packages-final.json'
    candidate.write_text(json.dumps(catalog, indent=2) + '\n')
    if (STAGE / 'packages-final.json.asc').exists():
        (STAGE / 'packages-final.json.asc').unlink()
    run('gpg', '--homedir', KEYHOME, '--batch', '--local-user', KEY, '--armor',
        '--detach-sign', '--output', str(STAGE / 'packages-final.json.asc'), str(candidate))
    run('gpgv', '--keyring', str(PUBLIC / 'goblinreactor-archive-keyring.gpg'),
        str(STAGE / 'packages-final.json.asc'), str(candidate))
    readme = render_readme(catalog, date)
    (STAGE / 'README.md').write_text(readme)
    page = HTML_HEAD + render_markdown(readme) + HTML_FOOT.format(date=date)
    (STAGE / 'index.html').write_text(page)
    atomic_copy(candidate, PUBLIC / 'packages.json')
    atomic_copy(STAGE / 'packages-final.json.asc', PUBLIC / 'packages.json.asc')
    atomic_copy(STAGE / 'README.md', PUBLIC / 'README.md')
    atomic_copy(STAGE / 'README.md', PUBLIC / 'README.txt')
    atomic_copy(STAGE / 'index.html', PUBLIC / 'index.html')
    print(f'PASS: catalog lists {len(catalog["packages"])} packages; README.md, README.txt and index.html regenerated')


if __name__ == '__main__':
    assert len(sys.argv) == 2 and sys.argv[1] in ('packages', 'docs'), __doc__
    STAGE.mkdir(mode=0o700, exist_ok=True)
    (publish_packages if sys.argv[1] == 'packages' else publish_docs)()
