#!/bin/sh
# Build odin-seat for one Debian or Ubuntu release. Runs only in the
# disposable packaging container (see Containerfile); /out is its artifact
# mount. Arguments: the suite codename and the full package version.
set -eu
test "$#" -eq 2
suite=$1
version=$2
case "$suite" in jammy|noble|resolute|bookworm|trixie) ;; *) exit 2 ;; esac
dpkg --validate-version "$version"
test -d /out
test ! -e /build
mkdir /build
tar -xzf /tmp/goblin-snapshot.tar.gz -C /build
cd /build/odin-seat
export DEBFULLNAME='Adam DePrince' DEBEMAIL='adam.deprince@gmail.com'
dch --newversion "$version" --distribution "$suite" --force-distribution \
    "Build against the native $suite distribution libraries."
# Build as an unprivileged user: dpkg-buildpackage needs no root, and the
# end-to-end tests want an ordinary user's shell prompt.
useradd -m -s /bin/bash builder
chown -R builder /build
runuser -u builder -- sh -c 'cd /build/odin-seat && dpkg-buildpackage -us -uc -j"$(nproc)"'
deb="/build/odin-seat_${version}_$(dpkg --print-architecture).deb"
test -f "$deb"
apt-get install -y --no-install-recommends "$deb"
/usr/bin/odin-seat -V
# The installed package, end to end: a detached session, a scripted menu
# key, the command-line query, the manual pages.
export HOME=/build/home
mkdir -p "$HOME"
odin-seat -L pkgtest new-session -s t -d -x 100 -y 30 -- sleep 30
odin-seat -L pkgtest menu o
test "$(odin-seat -L pkgtest list-windows -s t | wc -l)" -eq 2
odin-seat -L pkgtest kill-server
test -f /usr/share/odin-seat/pinyin.dict
man -w odin-seat odin-seat-protocols >/dev/null
cp /build/*.deb /build/*.dsc /build/*.tar.xz /build/*.buildinfo /build/*.changes /out/
dpkg-deb --info "$deb" > /out/package-info.txt
dpkg-deb --contents "$deb" > /out/package-contents.txt
dpkg-query -W > /out/build-packages.txt
printf 'Package built, installed and exercised: %s\n' "$version"
