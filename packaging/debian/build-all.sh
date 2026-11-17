#!/bin/sh
# Build odin-seat packages for every supported Debian and Ubuntu release
# in disposable podman containers. Run on a Linux host with podman, from a
# directory holding snapshot.tar.gz (one odin-seat/ tree inside), this
# script, build-release.sh and Containerfile. Arguments: the base version
# (e.g. 0.1.0+20260910.1) and the suites to build (default: all).
set -eu
base=$1
shift
suites=${*:-"jammy noble resolute bookworm trixie"}
image_for() {
    case "$1" in
    jammy) echo docker.io/library/ubuntu:22.04 ;;
    noble) echo docker.io/library/ubuntu:24.04 ;;
    resolute) echo docker.io/library/ubuntu:26.04 ;;
    bookworm) echo docker.io/library/debian:12 ;;
    trixie) echo docker.io/library/debian:13 ;;
    *) echo "unknown suite $1" >&2; exit 2 ;;
    esac
}
for suite in $suites; do
    img=$(image_for "$suite")
    version="${base}+${suite}1"
    out="$PWD/artifacts/$suite"
    rm -rf "$out"
    mkdir -p "$out"
    podman pull -q "$img" >/dev/null
    podman image inspect --format '{{.Digest}}' "$img" > "$out/base-image-digest.txt"
    echo "$img" >> "$out/base-image-digest.txt"
    podman build -q --build-arg BASE_IMAGE="$img" -f Containerfile \
        -t "odin-seat-build:$suite" . >/dev/null
    podman run --rm -v "$out:/out:Z" "odin-seat-build:$suite" \
        "$suite" "$version" > "$out/build.log" 2>&1 || {
        echo "FAILED $suite (see $out/build.log)"; tail -40 "$out/build.log"; exit 1; }
    echo "built $suite: $(ls "$out"/*.deb)"
done
