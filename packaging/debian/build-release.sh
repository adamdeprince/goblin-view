#!/bin/sh
# Build goblin-view for one Debian or Ubuntu release. Runs only in the
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
cd /build/goblin-view
export DEBFULLNAME='Adam DePrince' DEBEMAIL='adam.deprince@gmail.com'
dch --newversion "$version" --distribution "$suite" --force-distribution \
    "Build against the native $suite distribution libraries."
# Build as an unprivileged user: dpkg-buildpackage needs no root, and the
# end-to-end tests want an ordinary user's shell prompt.
useradd -m -s /bin/bash builder
chown -R builder /build
runuser -u builder -- sh -c 'cd /build/goblin-view && dpkg-buildpackage -us -uc -j"$(nproc)"'
deb="/build/goblin-view_${version}_$(dpkg --print-architecture).deb"
test -f "$deb"
apt-get install -y --no-install-recommends "$deb"
/usr/bin/goblin-view -V
# The installed package, end to end: a detached session, a scripted menu
# key, the command-line query, the manual pages.
export HOME=/build/home
mkdir -p "$HOME"
goblin-view -L pkgtest new-session -s t -d -x 100 -y 30 -- sleep 30
goblin-view -L pkgtest menu o
test "$(goblin-view -L pkgtest list-windows -s t | wc -l)" -eq 2
goblin-view -L pkgtest kill-server
test -f /usr/share/goblin-view/pinyin.dict
man -w goblin-view goblin-view-protocols >/dev/null
cp /build/*.deb /build/*.dsc /build/*.tar.xz /build/*.buildinfo /build/*.changes /out/
dpkg-deb --info "$deb" > /out/package-info.txt
dpkg-deb --contents "$deb" > /out/package-contents.txt
dpkg-query -W > /out/build-packages.txt
printf 'Package built, installed and exercised: %s\n' "$version"
