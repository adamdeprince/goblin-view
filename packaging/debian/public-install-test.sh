#!/bin/sh
# Install goblin-view from the public signed repository in a fresh container
# of each release and exercise it, the way a user would. Run on a podman
# host after publish.py packages; writes artifacts/<suite>/public-install.log.
set -eu
suites=${*:-"jammy noble resolute bookworm trixie"}
image_for() {
    case "$1" in
    jammy) echo docker.io/library/ubuntu:22.04 ;;
    noble) echo docker.io/library/ubuntu:24.04 ;;
    resolute) echo docker.io/library/ubuntu:26.04 ;;
    bookworm) echo docker.io/library/debian:12 ;;
    trixie) echo docker.io/library/debian:13 ;;
    esac
}
for suite in $suites; do
    log="$PWD/artifacts/$suite/public-install.log"
    podman run --rm "$(image_for "$suite")" sh -c '
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends ca-certificates curl gnupg >/dev/null
. /etc/os-release
case "$ID:$VERSION_CODENAME" in
    ubuntu:jammy|ubuntu:noble|ubuntu:resolute|debian:bookworm|debian:trixie) ;;
    *) echo "unsupported $ID:$VERSION_CODENAME" >&2; exit 1 ;;
esac
test "$(dpkg --print-architecture)" = amd64
keydir=$(mktemp -d)
curl -fsSL https://apt.goblinreactor.com/goblinreactor-archive-keyring.gpg -o "$keydir/keyring.gpg"
fpr=$(gpg --batch --show-keys --with-colons "$keydir/keyring.gpg" | awk -F: "\$1 == \"fpr\" { print \$10; exit }")
test "$fpr" = B7B61E8CB85A9270530364F23273CB62402FC793
install -d -m 0755 /etc/apt/keyrings
install -m 0644 "$keydir/keyring.gpg" /etc/apt/keyrings/goblinreactor-archive-keyring.gpg
printf "%s\n" "Types: deb deb-src" "URIs: https://apt.goblinreactor.com/" "Suites: $VERSION_CODENAME" \
    "Components: main" "Architectures: amd64" \
    "Signed-By: /etc/apt/keyrings/goblinreactor-archive-keyring.gpg" \
    > /etc/apt/sources.list.d/goblinreactor.sources
apt-get update -qq
apt-cache policy goblin-view
apt-get install -y -qq goblin-view
goblin-view -V
dpkg-query -W goblin-view
export HOME=/root
set -x
goblin-view -L t new-session -s s -d -x 100 -y 30 -- sleep 30
goblin-view -L t menu o
test "$(goblin-view -L t list-windows -s s | wc -l)" -eq 2
goblin-view -L t list-clients >/dev/null
goblin-view -L t kill-server
# What the package ships (a minimized image may exclude /usr/share/man).
dpkg -L goblin-view | grep -q "^/usr/share/man/man1/goblin-view.1.gz$"
dpkg -L goblin-view | grep -q "^/usr/share/man/man7/goblin-view-protocols.7.gz$"
test -f /usr/share/goblin-view/pinyin.dict
test -f /usr/share/goblin-view/wubi.dict
apt-get install -y -qq dpkg-dev >/dev/null
mkdir /src && cd /src && apt-get source -qq goblin-view
test -d /src/goblin-view-*/debian
echo "PASS: public signed APT install and smoke test: $VERSION_CODENAME $(dpkg --print-architecture)"
' > "$log" 2>&1 || { echo "FAILED $suite"; tail -30 "$log"; exit 1; }
    tail -1 "$log"
done
