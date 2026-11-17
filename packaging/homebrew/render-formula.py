#!/usr/bin/env python3
"""Render Formula/odin-seat.rb from odin-seat.rb.in.

    render-formula.py odin-seat-<version>.tar.gz [bottle.json]

The archive's SHA-256 and version fill the template; a bottle JSON file
(from `brew bottle --json`) adds the bottle block with its tag and checksum.
"""
import hashlib
import json
import pathlib
import re
import sys

archive = pathlib.Path(sys.argv[1])
version = re.fullmatch(r'odin-seat-(.+)\.tar\.gz', archive.name).group(1)
sha = hashlib.sha256(archive.read_bytes()).hexdigest()
block = ''
if len(sys.argv) > 2:
    data = json.loads(pathlib.Path(sys.argv[2]).read_text())
    bottle = next(iter(data.values()))['bottle']
    tags = bottle['tags']
    # The cellar is per bottle: a symbol name (any, any_skip_relocation) or,
    # for a bottle usable only in the Cellar it was built in, a literal path.
    cellar = bottle['cellar'].lstrip(':')
    cellar_rb = f'"{cellar}"' if cellar.startswith('/') else f':{cellar}'
    lines = ['  bottle do', f'    root_url "{bottle["root_url"]}"']
    for tag, t in tags.items():
        lines.append(f'    sha256 cellar: {cellar_rb}, {tag}: "{t["sha256"]}"')
    lines.append('  end')
    block = '\n'.join(lines) + '\n\n'
template = (pathlib.Path(__file__).parent / 'odin-seat.rb.in').read_text()
out = template.replace('@VERSION@', version).replace('@SOURCE_SHA256@', sha)
out = out.replace('@BOTTLE_BLOCK@\n', block)
sys.stdout.write(out)
