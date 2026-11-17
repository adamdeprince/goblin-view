#!/usr/bin/env python3
"""Executable and dictionary discovery after relocation, including long paths."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
binary = Path(os.environ.get("GV", str(source / "odin-seat"))).resolve()
env = dict(os.environ)
env.pop("ODIN_SEAT_IMDATA", None)


def check(prefix, workdir):
    bindir = prefix / "bin"
    bindir.mkdir(parents=True)
    datadir = prefix / "share" / "odin-seat"
    shutil.copytree(source / "share", datadir)
    for program in (binary, source / "test-im"):
        shutil.copy2(program, bindir / program.name)
    output = subprocess.check_output(
        [str(bindir / "odin-seat"), "-V"], cwd=workdir, env=env, text=True
    )
    reported = next(line[len("data directory: "):]
                    for line in output.splitlines()
                    if line.startswith("data directory: "))
    assert Path(reported).resolve() == datadir.resolve(), output
    subprocess.run([str(bindir / "test-im")], cwd=workdir, env=env, check=True)
    link = workdir / "odin-seat-link"
    link.symlink_to(bindir / "odin-seat")
    try:
        linked = subprocess.check_output([str(link), "-V"], cwd=workdir,
                                         env=env, text=True)
        assert linked == output, (linked, output)
    finally:
        link.unlink()


with tempfile.TemporaryDirectory(prefix="odin-seat-paths-") as temp:
    root = Path(temp).resolve()
    check(root / "relocated", root)
    prefix = root / "long"
    # Exercise paths beyond the former 1024-byte buffers where the OS permits.
    while len(os.fsencode(prefix / "bin" / "test-im")) <= 1024:
        prefix /= "nested-" + "x" * 80
    path_max = os.pathconf(root, "PC_PATH_MAX")
    if path_max < 0 or len(os.fsencode(prefix / "share/odin-seat/pinyin.dict")) < path_max:
        check(prefix, root)
        print("regress/paths.py: relocation, symlinks and paths over 1024 bytes: ok")
    else:
        print("regress/paths.py: relocation and symlinks: ok; long path exceeds OS PATH_MAX")
