#!/usr/bin/env python3
"""Publish a goblin-view Homebrew release on hail.

Runs on hail. The stage directory holds the immutable source archive (already
under sources/), the bottle (renamed to one dash), the final formula,
bottle.json, the tap's commit hash and the verification logs. This script:

    publish-brew.py artifacts   # formula, bottle.json, build notes into releases/
    publish-brew.py docs        # test logs into releases/, packages.json (signed), pages

The bottle itself is copied into bottles/ by hand first (it must be public
before the public-install test that `docs` requires can run).

The pages are generated from the catalog, so they list every formula the tap
publishes (goblin-mosh included).
"""
import hashlib
import html
import json
import os
import pathlib
import re
import subprocess
import sys

VERSION = '0.1.0-20260911.4'
PACKAGE = 'goblin-view'
TAG = 'arm64_tahoe'
STAGE = pathlib.Path.home() / '.local/share/goblin-apt' / 'goblin-view-brew-20260911'
PUBLIC = pathlib.Path('/mnt/distribution/brew.goblinreactor.com')
KEYHOME = str(pathlib.Path.home() / '.local/share/goblin-apt/gnupg')
KEY = 'B7B61E8CB85A9270530364F23273CB62402FC793'
BASE = 'https://brew.goblinreactor.com/'
RELEASE_DIR = f'releases/{PACKAGE}-{VERSION}'
BOTTLE = f'{PACKAGE}-{VERSION}.{TAG}.bottle.tar.gz'
SOURCE = f'{PACKAGE}-{VERSION}.tar.gz'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, **kw):
    print('+', ' '.join(args))
    return subprocess.run(args, check=True, **kw)


def atomic_copy(source, target, mode=0o644):
    target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    temporary = target.with_name(target.name + '.goblin-view-brew.tmp')
    assert not temporary.exists(), temporary
    with temporary.open('xb') as stream:
        stream.write(source.read_bytes())
    temporary.chmod(mode)
    os.replace(temporary, target)


def bottle_info():
    data = json.loads((STAGE / 'bottle.json').read_text())
    entry = next(iter(data.values()))
    tags = entry['bottle']['tags']
    assert list(tags) == [TAG], tags
    return entry, tags[TAG]


def publish_artifacts():
    entry, tag = bottle_info()
    bottle = STAGE / BOTTLE
    assert bottle.is_file() and sha256(bottle) == tag['sha256'], 'staged bottle does not match bottle.json'
    assert (PUBLIC / 'sources' / SOURCE).is_file(), 'publish the source archive first'
    formula = (STAGE / 'goblin-view.rb').read_text()
    assert tag['sha256'] in formula and sha256(PUBLIC / 'sources' / SOURCE) in formula, \
        'the staged formula does not reference the staged bottle and source'
    public_bottle = PUBLIC / 'bottles' / BOTTLE
    assert public_bottle.is_file() and sha256(public_bottle) == tag['sha256'], 'publish the bottle first'
    for name in ('goblin-view.rb', 'bottle.json', 'build-environment.txt', 'build-notes.txt'):
        atomic_copy(STAGE / name, PUBLIC / RELEASE_DIR / name)
    print(f'PASS: bottle bottles/{BOTTLE} matches; formula and build notes under {RELEASE_DIR}/')


def inventory():
    """Every public file a visitor can download, with checksums."""
    out = []
    for path in sorted(PUBLIC.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(PUBLIC)
        if rel.parts[0] == 'homebrew-tap.git' or rel.name in ('packages.json', 'packages.json.asc'):
            continue
        if rel.suffix == '.html':	# the CDN injects its analytics script into HTML
            continue
        out.append({'path': str(rel), 'filename': path.name, 'url': BASE + str(rel),
                    'size': path.stat().st_size, 'sha256': sha256(path)})
    return out


def render_markdown(md):
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
            out.append(f'<h1 id="{slug(line[2:])}">{inline(line[2:])}</h1>'); i += 1
        elif line.startswith('## '):
            out.append(f'<h2 id="{slug(line[3:])}">{inline(line[3:])}</h2>'); i += 1
        elif line.startswith('### '):
            out.append(f'<h3 id="{slug(line[4:])}">{inline(line[4:])}</h3>'); i += 1
        elif line.startswith('~~~'):
            i += 1
            block = []
            while i < len(lines) and not lines[i].startswith('~~~'):
                block.append(lines[i]); i += 1
            i += 1
            out.append('<pre><code>' + html.escape('\n'.join(block)) + '</code></pre>')
        elif line.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].startswith('|'):
                rows.append([c.strip() for c in lines[i].strip().strip('|').split('|')]); i += 1
            th = ''.join(f'<th scope="col">{inline(c)}</th>' for c in rows[0])
            trs = ''.join('<tr>' + ''.join(f'<td>{inline(c)}</td>' for c in r) + '</tr>' for r in rows[2:])
            out.append(f'<div class="table-wrap"><table><thead><tr>{th}</tr></thead><tbody>{trs}</tbody></table></div>')
        elif line.strip() == '':
            i += 1
        else:
            para = [line.strip()]; i += 1
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(('#', '~~~', '|')):
                para.append(lines[i].strip()); i += 1
            out.append('<p>' + inline(' '.join(para)) + '</p>')
    return '\n'.join(out)


HTML_HEAD = '''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Goblin Reactor Homebrew packages</title><meta name="description" content="Goblin Reactor Homebrew tap for macOS: goblin-mosh and goblin-view formulas, bottles, matching source, checksums and installation instructions."><style>
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
</style></head><body><main><nav aria-label="Repository"><a href="#available-packages">Packages</a><a href="#install">Install</a><a href="#artifacts-and-matching-source">Source</a><a href="README.txt">Plain text</a><a href="packages.json">Checksummed catalog</a><a href="https://apt.goblinreactor.com/">Linux packages</a></nav>
'''
HTML_FOOT = '''<footer>Goblin Reactor · Updated {date} · <a href="https://brew.goblinreactor.com/goblinreactor-archive-keyring.asc">Public signing key</a></footer></main></body></html>
'''


def render_readme(catalog, date):
    by = {p['name']: p for p in catalog['packages']}
    mosh, view = by['goblin-mosh'], by['goblin-view']
    md = []
    md.append('# Goblin Reactor Homebrew packages\n')
    md.append('Goblin Mosh and GoblinView for macOS, maintained by Adam DePrince. This is an independent '
              'third-party Homebrew tap, hosted at https://brew.goblinreactor.com/. Linux users have '
              'the [Debian, Ubuntu and RPM packages](https://apt.goblinreactor.com/).\n')
    md.append('## Available packages\n')
    md.append('| Package | Version | Tested binary target |\n| --- | --- | --- |')
    for p in (mosh, view):
        md.append(f"| {p['formula']} | {p['version']} | Apple Silicon ({p['architecture']}), {p['distribution']} |")
    md.append('')
    md.append('The bottles are the tested prebuilt packages. Other macOS targets can attempt a source '
              'build, but have not been verified for these releases. Do not force an ARM bottle onto an '
              'Intel Mac or an older macOS release.\n')
    md.append('### goblin-mosh\n')
    md.append('A Mosh fork for bandwidth-constrained links: zstd level 22, independent upload/download '
              'pacing, idle keepalive backoff, terminal graphics, and a two-pane file-transfer menu with '
              'librsync and FEC. It includes goblin-mosh, goblin-mosh-client, goblin-mosh-server, '
              'goblin-moshcp, the dictionary compiler, five man pages and Bash completion, bundled in one '
              'formula.\n')
    md.append('### goblin-view\n')
    md.append('A terminal multiplexer with overlapping windows and an Alt-tap menu, not tiled panes. Each '
              'window is a terminal of its own kept in a server process, so a session survives '
              'detaching; the mouse moves, resizes, selects and scrolls; images drawn with kitty graphics '
              'or sixel are re-emitted in whatever the terminal you attach from speaks; and programs in '
              'windows get the kitty protocol extensions per window (keyboard protocol, styled '
              'underlines, colours, pointer shapes, extra cursors, drag and drop, file transfer). It runs '
              'best in Ghostty or kitty. The formula installs `goblin-view`, the manual pages '
              '`goblin-view(1)` and `goblin-view-protocols(7)`, and the input-method dictionaries. It '
              'has no Homebrew dependencies beyond the system zlib.\n')
    md.append('## Install\n')
    md.append('Install [Homebrew](https://brew.sh/) first if it is not already installed, then run '
              'these commands as your normal user, without sudo:\n')
    md.append('~~~sh\nbrew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git\n'
              'brew install goblinreactor/tap/goblin-view\nbrew install goblinreactor/tap/goblin-mosh\n~~~\n')
    md.append('The explicit HTTPS tap URL is important: this tap is hosted here, not in a GitHub '
              'repository inferred from its name. Homebrew downloads a matching bottle automatically and '
              'verifies the source and bottle with the SHA-256 checksums in the formula; TLS '
              'verification must remain enabled. Homebrew installs the runtime dependencies of '
              'goblin-mosh (protobuf, abseil, OpenSSL 3, zstd, librsync, WebP, PNG and ncurses); '
              'goblin-view needs none. Do not use --ignore-dependencies or --force-bottle as a '
              'workaround for an unsupported machine.\n')
    md.append('This is third-party system software. Adding a tap means trusting its maintainer and '
              'formula code, including future updates. No custom installation script or root-level '
              'background service is required.\n')
    md.append('## Use\n')
    md.append('### goblin-view\n')
    md.append('Run `goblin-view` in a terminal to attach to a session, creating it with one window if '
              'none exists. Tap Alt (Option, either side) and let go to open the menu, then a letter: '
              'O opens a window, C closes, Z zooms, R rearranges, 1-9 selects, D detaches, Q quits, ? '
              'shows help. In Ghostty on macOS set `macos-option-as-alt = true` in '
              '`~/.config/ghostty/config` so the Option tap arrives; `Ctrl-\\` opens the menu on a '
              'terminal that swallows it. `man goblin-view` covers the command, keys, mouse and '
              'environment; `man goblin-view-protocols` lists what programs in windows can use.\n')
    md.append('~~~sh\ngoblin-view\ngoblin-view new-window -- htop\ngoblin-view list-windows\n'
              'goblin-view kill-server\n~~~\n')
    md.append('A goblin-view server keeps running across upgrades; a newer client that attaches to it '
              'upgrades it in place (`goblin-view upgrade` does the same by hand), keeping every window '
              'and attached terminal. A server too old for that is refused with a message; close its '
              'windows and run `goblin-view kill-server`. A manual copy under ~/.local/bin can shadow '
              'the Homebrew one: check `command -v goblin-view`.\n')
    md.append('### goblin-mosh\n')
    md.append('Install Goblin Mosh on both endpoints, then connect:\n')
    md.append('~~~sh\ngoblin-mosh user@server\nman goblin-mosh\n~~~\n')
    md.append('Open the two-pane control/file menu with Mosh\'s command prefix: Ctrl-^ (usually '
              'Ctrl-Shift-6), then 0. Enter starts transferring the selected file or directory without '
              'leaving the browser. SSH must reach the server for login, and its firewall must allow the '
              'selected Mosh UDP port (normally 60000–61000). The goblin-prefixed commands coexist with '
              'Homebrew\'s ordinary mosh formula and do not delete manually installed files; check which '
              'installation your shell selects with `command -v goblin-mosh`. On a remote Apple Silicon '
              'Mac whose SSH PATH excludes Homebrew, select the server explicitly: '
              '`goblin-mosh --server=/opt/homebrew/bin/goblin-mosh-server user@mac`.\n')
    md.append('## Update, verify and remove\n')
    md.append('~~~sh\nbrew update\nbrew upgrade goblinreactor/tap/goblin-view goblinreactor/tap/goblin-mosh\n'
              'brew info goblinreactor/tap/goblin-view\nbrew test goblinreactor/tap/goblin-view\n~~~\n')
    md.append('The tap is a small, read-only Git repository over HTTPS; normal `brew update` fetches '
              'its formula changes from this server. goblin-view\'s installed-package test starts a '
              'detached session on a private socket, opens a window with a scripted menu key and shuts '
              'the server down. goblin-mosh\'s test needs its Python test dependency: '
              '`brew install --only-dependencies --include-test goblinreactor/tap/goblin-mosh`.\n')
    md.append('~~~sh\nbrew uninstall goblin-view\nbrew uninstall goblin-mosh\nbrew untap goblinreactor/tap\n~~~\n')
    md.append('## Artifacts and matching source\n')
    md.append('| Package | Artifact | SHA-256 |\n| --- | --- | --- |')
    inv = {a['url']: a for a in catalog['artifacts']}
    for p in (view, mosh):
        for label, url in (('source archive', p['source']), (f"bottle ({p['platform_tag']})", p['bottle'])):
            a = inv[url]
            md.append(f"| {p['name']} | [{label}]({url}) | `{a['sha256']}` |")
        rel = p.get('release_files')
        if rel:
            md.append(f"| {p['name']} | [formula and verification logs]({rel}) | |")
    md.append('')
    md.append('Homebrew checks the formula\'s SHA-256 values automatically. The '
              '[checksummed catalog](https://brew.goblinreactor.com/packages.json) lists every public '
              'file with its checksum and is signed with the Goblin Reactor archive key '
              '([signature](https://brew.goblinreactor.com/packages.json.asc), '
              '[public key](https://brew.goblinreactor.com/goblinreactor-archive-keyring.asc)):\n')
    md.append('~~~text\nB7B6 1E8C B85A 9270 5303 64F2 3273 CB62 402F C793\n~~~\n')
    md.append('The catalog covers every downloadable file except the HTML pages: index.html is a '
              'rendering of README.md, and the CDN in front of this site adds its analytics script to '
              'HTML in transit, so README.md and README.txt are the checksummed texts. '
              'That signature is an additional verification mechanism, not one Homebrew itself checks. '
              'Each source archive contains the complete source, build system, licenses and packaging '
              'recipes; the exact formula is in the tap and recorded inside each bottle. To compile a '
              'package yourself instead of using its bottle:\n')
    md.append('~~~sh\nbrew install --build-from-source goblinreactor/tap/goblin-view\n~~~\n')
    md.append('Source builds need current Apple Command Line Tools. goblin-view\'s build runs its unit '
              'tests; goblin-mosh\'s runs 24 core tests. Each bottle was verified by installing it '
              'through the public HTTPS tap and running the formula\'s test.\n')
    md.append('## Licenses and limits\n')
    md.append('GoblinView is licensed under the Apache License 2.0; its bundled input-method '
              'dictionaries are generated from MIT-, Apache-2.0- and Unicode-licensed data, recorded in '
              'the installed NOTICE and share/goblin-view/README.md. Goblin Mosh remains '
              'GPL-3.0-or-later with upstream notices and OpenSSL linking exceptions preserved; see its '
              'installed COPYING, copyright and THIRD_PARTY.md. Homebrew dependencies are separately '
              'maintained, dynamically linked packages.\n')
    md.append('Goblin Mosh: normal macOS sessions use Apple\'s CommonCrypto backend; the --fips-crypto '
              'provider mode is built using OpenSSL 3 but no validated provider is included. Source '
              'branch: https://github.com/adamdeprince/mosh/tree/low-bandwidth-mosh. GoblinView has no '
              'public repository yet; its complete source is the archive above.\n')
    md.append('Homebrew tap documentation: https://docs.brew.sh/How-to-Create-and-Maintain-a-Tap\n')
    return '\n'.join(md)


def publish_docs():
    entry, tag = bottle_info()
    for log in ('source-build-test.log', 'public-bottle-install.log', 'public-bottle-test.log'):
        assert (STAGE / log).is_file(), f'{log}: run the tests first'
        assert 'PASS' in (STAGE / log).read_text(), log
        atomic_copy(STAGE / log, PUBLIC / RELEASE_DIR / log)
    catalog = json.loads((PUBLIC / 'packages.json').read_text())
    tap_commit = (STAGE / 'tap-commit.txt').read_text().strip()
    assert re.fullmatch(r'[0-9a-f]{40}', tap_commit), tap_commit
    public_commit = subprocess.check_output(['git', '--git-dir', str(PUBLIC / 'homebrew-tap.git'),
                                             'rev-parse', 'refs/heads/main'], text=True).strip()
    assert public_commit == tap_commit, f'public tap is at {public_commit}, stage says {tap_commit}'
    package = {
        'name': PACKAGE, 'formula': f'goblinreactor/tap/{PACKAGE}', 'format': 'homebrew-bottle',
        'version': VERSION, 'distribution': 'macOS 26 Tahoe', 'architecture': 'arm64',
        'platform_tag': TAG, 'cellar': entry['bottle']['cellar'].lstrip(':'),
        'source': BASE + 'sources/' + SOURCE, 'bottle': BASE + 'bottles/' + BOTTLE,
        'release_files': BASE + RELEASE_DIR + '/',
        'runtime_dependencies': entry['bottle'].get('runtime_dependencies', []) or
                                entry.get('runtime_dependencies', []),
        'verification': {'unit_tests': 'pass', 'source_build_test': 'pass',
                         'public_https_bottle_install': 'pass', 'public_bottle_test': 'pass'},
    }
    catalog['packages'] = [p for p in catalog['packages'] if p['name'] != PACKAGE] + [package]
    date = subprocess.check_output(['date', '-u', '+%Y-%m-%d'], text=True).strip()
    catalog['updated'] = date
    catalog['tap_commit'] = tap_commit
    # Pages first, then the inventory that lists them, then the signed catalog.
    catalog['artifacts'] = inventory()
    readme = render_readme(catalog, date)
    (STAGE / 'README.md').write_text(readme)
    (STAGE / 'index.html').write_text(HTML_HEAD + render_markdown(readme) + HTML_FOOT.format(date=date))
    atomic_copy(STAGE / 'README.md', PUBLIC / 'README.md')
    atomic_copy(STAGE / 'README.md', PUBLIC / 'README.txt')
    atomic_copy(STAGE / 'index.html', PUBLIC / 'index.html')
    atomic_copy(STAGE / 'README.md', PUBLIC / RELEASE_DIR / 'README.md')
    catalog['artifacts'] = inventory()
    for p in catalog['packages']:
        for url in (p['source'], p['bottle']):
            assert any(a['url'] == url for a in catalog['artifacts']), url
    catalog['verified'] = subprocess.check_output(['date', '-u', '+%Y-%m-%dT%H:%M:%SZ'], text=True).strip()
    candidate = STAGE / 'packages-final.json'
    candidate.write_text(json.dumps(catalog, indent=2) + '\n')
    sig = STAGE / 'packages-final.json.asc'
    if sig.exists():
        sig.unlink()
    run('gpg', '--homedir', KEYHOME, '--batch', '--local-user', KEY, '--armor', '--detach-sign',
        '--output', str(sig), str(candidate))
    run('gpgv', '--keyring', str(PUBLIC / 'goblinreactor-archive-keyring.gpg'), str(sig), str(candidate))
    atomic_copy(candidate, PUBLIC / 'packages.json')
    atomic_copy(sig, PUBLIC / 'packages.json.asc')
    print(f'PASS: catalog lists {len(catalog["packages"])} packages and {len(catalog["artifacts"])} files; pages regenerated')


if __name__ == '__main__':
    assert len(sys.argv) == 2 and sys.argv[1] in ('artifacts', 'docs'), __doc__
    (publish_artifacts if sys.argv[1] == 'artifacts' else publish_docs)()
