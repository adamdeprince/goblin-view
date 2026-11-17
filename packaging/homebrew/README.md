# Homebrew packaging

For the automatic macOS installer and manual installation commands, see
the [project installation instructions](../../README.md#install).
The installer is served from `html/install.sh` and bootstraps Homebrew if needed.
Run `make test-installer` for isolated installer regressions; the tests mock
network requests and package managers without installing software on the host.

Odin Seat uses the `odin-seat` formula. `odin-seat.rb.in` is the
formula template; `render-formula.py` fills in the version, source
archive checksum and optional bottle metadata:

```sh
python3 packaging/homebrew/render-formula.py odin-seat-<version>.tar.gz > odin-seat.rb
python3 packaging/homebrew/render-formula.py odin-seat-<version>.tar.gz bottle.json > odin-seat.rb
```

The formula builds with the Makefile, runs the unit tests, and installs
`odin-seat`, its manual pages, and the input-method
dictionaries. The relative `DATADIR_COMPILED=../share/odin-seat` keeps
bottles relocatable. `odin-seat -V` reports the resolved data directory.

`brew test` checks the installed command, dictionaries and manuals, then starts
a detached session on a private socket, opens another window, and shuts
the server down. The formula's public homepage is
https://xn--hliskjlf-eza0m.goblinreactor.com/.
