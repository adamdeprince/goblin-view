# Debian and Ubuntu packages

The package is `goblin-view`. It installs `/usr/bin/goblin-view`, the
manual pages, and the input-method dictionaries under
`/usr/share/goblin-view`. `debian/` at the top of the tree is the
packaging (debhelper 13, native source format); this directory holds the
container recipes that build it for every supported release and the
scripts that publish it.

Builds are made separately for Ubuntu 22.04 (`jammy`), 24.04 (`noble`),
26.04 (`resolute`), Debian 12 (`bookworm`) and Debian 13 (`trixie`), in
disposable podman containers from the official base images, and the
release name is part of the package version:
`0.1.0+20260910.1+noble1`. Different releases' builds never share an APT
suite. The package build runs the unit tests, the socket-level skeleton
and the end-to-end pseudo-terminal tests, installs the resulting `.deb`
in the container and exercises it.

## Build

On a Linux host with podman (naamah), from a directory holding
`snapshot.tar.gz` (one `goblin-view/` tree: the sources, `debian/` and
this directory, without build products), plus copies of `build-all.sh`,
`build-release.sh` and `Containerfile`:

```sh
./build-all.sh 0.1.0+20260910.1            # every suite
./build-all.sh 0.1.0+20260910.1 noble      # one
```

`artifacts/<suite>/` then holds the `.deb`, `.dsc`, source archive,
`.buildinfo`, `.changes`, the build log, the package's file list, the
build container's package inventory and the pinned base-image digest.

## Publish

The repository is https://apt.goblinreactor.com/, served from
`/mnt/distribution/apt.goblinreactor.com` on hail. Its private reprepro
configuration and the archive signing key (fingerprint
`B7B6 1E8C B85A 9270 5303 64F2 3273 CB62 402F C793`) live under
`~/.local/share/goblin-apt/` on hail, outside the public root; keep them
there.

1. Copy `artifacts/` to
   `~/.local/share/goblin-apt/goblin-view-release-<date>/artifacts/` on
   hail and this `publish.py` beside it (edit `RELEASE` and
   `VERSION_BASE` at its top for a new release).
2. `python3 publish.py packages` checks every build's test results, adds
   each `.dsc` and `.deb` to its suite with reprepro, re-exports and
   verifies the signed indexes, and publishes the build logs under
   `releases/<date>/goblin-view/<suite>/`. Nothing already in the pool is
   replaced.
3. On the podman host, `./public-install-test.sh` installs the package
   from the public repository in a fresh container of each release with
   the documented setup block, exercises it, and fetches the matching
   source. Copy the resulting `public-install.log` files into the stage's
   `artifacts/<suite>/`.
4. `python3 publish.py docs` re-checks every artifact the public catalog
   already lists, adds the goblin-view entries, signs `packages.json`,
   and regenerates `README.md`, `README.txt` and `index.html` from the
   catalog so the pages list exactly what is published.

The documentation is generated from the catalog, so it lists every
package the repository holds, goblin-mosh included; rerun step 4 whenever
another package's release has rewritten the pages.
