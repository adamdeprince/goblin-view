# Debian and Ubuntu packaging

For the automatic Linux installer and manual installation commands, see
the [project installation instructions](../../README.md#install).
The installer is served from `html/install.sh`; it verifies the archive key,
signed release metadata, OS codename and architecture before adding APT sources.
Run `make test-installer` for isolated installer regressions; the tests mock
network requests and package managers without installing software on the host.

The package is `odin-seat`. It installs `/usr/bin/odin-seat`, manual pages,
and input-method dictionaries under `/usr/share/odin-seat`.
The top-level `debian/` directory contains the debhelper 13 packaging.

Build from the source checkout with:

```sh
dpkg-buildpackage -us -uc -b
```

For release-specific builds, this directory contains disposable Podman
container recipes for Ubuntu 22.04 (`jammy`), 24.04 (`noble`),
26.04 (`resolute`), Debian 12 (`bookworm`) and Debian 13 (`trixie`).
From a directory containing `snapshot.tar.gz` (one `odin-seat/` source
tree), `build-all.sh`, `build-release.sh`, and `Containerfile`, run:

```sh
./build-all.sh 0.1.0+20261015.1
./build-all.sh 0.1.0+20261015.1 noble
```

`artifacts/<suite>/` holds the package, matching source, build metadata,
logs and test results. Each suite is built against its own distribution
libraries. The build exercises the installed command and checks the
manuals. `public-install-test.sh` verifies a published package through
the public signed repository at https://apt.goblinreactor.com/.
