# Homebrew release packaging

The public tap is `goblinreactor/tap`, hosted directly at
`https://brew.goblinreactor.com/homebrew-tap.git` (a bare Git repository
served statically; it needs `git update-server-info` after every push).
Its formulas are `goblin-view` and `goblin-mosh`. The web root on hail is
`/mnt/distribution/brew.goblinreactor.com`, with `sources/` (immutable
source archives), `bottles/`, `releases/<name>-<version>/` (formula,
bottle metadata, build notes, verification logs), the signed
`packages.json` catalog, and the generated README/index pages.

`goblin-view.rb.in` is the formula template; `render-formula.py` fills in
the version, the source archive's SHA-256 and, given `brew bottle`'s JSON,
the bottle block. The formula builds with the tree's Makefile, runs the
six unit-test programs in Homebrew's sandbox, and installs with
`DATADIR_COMPILED=../share/goblin-view`, so the binary embeds no
installation prefix and the bottle is relocatable
(`cellar: :any_skip_relocation`). At run time goblin-view finds
`share/goblin-view` beside its own `bin` directory; `goblin-view -V`
reports the directory it resolved. `brew test` starts a detached session
on a private socket, opens a window with a scripted menu key and shuts
the server down.

## Release steps

1. Snapshot the tree as `goblin-view-<version>.tar.gz` holding one
   `goblin-view-<version>/` directory (no build products, `.git`, or
   stray files), with a version like `0.1.0-20260911.4`. Upload it to
   `sources/` on hail; it is immutable from then on.
2. Render the source-only formula and put it in the tap checkout's
   `Formula/goblin-view.rb`.
3. Build the bottle on its actual target in a disposable Homebrew prefix
   (a clone of `/opt/homebrew`, see below) so the workstation's shared
   packages are untouched:
   `brew install --build-bottle --include-test goblinreactor/tap/goblin-view`,
   `brew test goblinreactor/tap/goblin-view`,
   `brew bottle --json --root-url=https://brew.goblinreactor.com/bottles goblinreactor/tap/goblin-view`.
   Check the JSON: the cellar must be `any` or `any_skip_relocation`,
   with no `binary_relocation_files`; a literal Cellar path means the
   binary embeds the build prefix and the bottle would work nowhere else.
4. Rename the bottle from `goblin-view--<version>.<tag>.bottle.tar.gz`
   to one dash, upload it to `bottles/`, render the formula again with
   the bottle JSON, and replace it in the tap checkout.
5. Verify the public bottle: in the disposable prefix, uninstall the
   source-built keg and `brew install goblinreactor/tap/goblin-view`
   (it must pour from HTTPS), then `brew test --verbose`. Keep the logs.
6. Commit the formula (and the tap README) with the maintainer's Git
   identity and push to `hail:/mnt/distribution/brew.goblinreactor.com/homebrew-tap.git`;
   run `git update-server-info` there. Check `git ls-remote` over HTTPS.
7. Stage `goblin-view.rb`, `bottle.json`, `build-environment.txt`
   (`brew config`), `build-notes.txt`, `source-build-test.log`,
   `public-bottle-install.log`, `public-bottle-test.log` and
   `tap-commit.txt` under `~/.local/share/goblin-apt/goblin-view-brew-<date>/`
   on hail with `publish-brew.py` (edit `VERSION` at its top), then run
   `python3 publish-brew.py artifacts` and `python3 publish-brew.py docs`.
   The docs step signs the catalog with the archive key and regenerates
   `README.md`, `README.txt` and `index.html` from it, so the pages list
   every formula the tap publishes.

## The disposable prefix

Homebrew refuses source builds on a Mac whose `/Applications/Xcode.app`
is older than the release it expects, even when the selected Command
Line Tools are current. The release builds therefore use a clone of the
Homebrew checkout under `/private/tmp/goblin-brew-release.*/homebrew`
with one local change in `Library/Homebrew/os/mac/xcode.rb`: when
`xcode-select` selects the Command Line Tools, `bundle_path` returns nil
instead of discovering the unused Xcode. This changes neither the
compiler nor the SDK, and the sandbox stays on. Run that prefix's `brew`
with `HOMEBREW_NO_AUTO_UPDATE`, `HOMEBREW_NO_INSTALL_CLEANUP`,
`HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK`, `HOMEBREW_NO_INSTALL_UPGRADE`
and its own `HOMEBREW_CACHE`, `HOMEBREW_LOGS` and `HOMEBREW_TEMP`. A
bottle built there is only publishable when it is relocatable (step 3).
