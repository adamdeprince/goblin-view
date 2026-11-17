#!/bin/sh
# Install Odin Seat through Homebrew (macOS) or the signed APT repository.
# Keep execution in main: a piped download must parse the functions before
# anything is installed. Child commands never read the incoming script.
set -eu

say() { printf '%s\n' "$@"; }
die() { printf 'odin-seat installer: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Install Odin Seat on macOS or an APT-based Linux distribution:
  curl -fsSL https://xn--hliskjlf-eza0m.goblinreactor.com/install.sh | sh

macOS: uses Homebrew, installing Homebrew first if needed.
Linux: adds the signed Goblin Reactor APT repository for your OS codename
       and architecture, then installs odin-seat. Uses sudo unless root.

Packages must be published for your platform. Other Linux distributions
can build from source; see the project README. Run odin-seat after install.
Options: -h, --help
EOF
}

download() {
    curl --proto '=https' --tlsv1.2 -fsSL "$1" -o "$2" </dev/null
}

as_root() {
    if [ "$(id -u)" = 0 ]; then
        "$@" </dev/null
    else
        sudo "$@" </dev/null
    fi
}

find_brew() {
    if command -v brew >/dev/null 2>&1; then
        command -v brew
    elif [ -x /opt/homebrew/bin/brew ]; then
        printf '%s\n' /opt/homebrew/bin/brew
    elif [ -x /usr/local/bin/brew ]; then
        printf '%s\n' /usr/local/bin/brew
    else
        return 1
    fi
}

install_macos() {
    [ "$(id -u)" != 0 ] || die 'Run this installer as your regular user on macOS, without sudo.'
    if ! brew_command=$(find_brew); then
        say 'Installing Homebrew using its official installer.'
        download https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh "$work/homebrew-install.sh"
        # Homebrew may ask for a password or confirmation. Read from the
        # terminal, never from the curl pipe. CI uses its noninteractive mode.
        if ( : </dev/tty ) 2>/dev/null; then
            /bin/bash "$work/homebrew-install.sh" </dev/tty
        else
            NONINTERACTIVE=1 /bin/bash "$work/homebrew-install.sh" </dev/null
        fi
        brew_command=$(find_brew) || die 'Homebrew was not found after setup. See https://brew.sh/.'
    fi
    say 'Installing Odin Seat with Homebrew.'
    "$brew_command" tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git </dev/null
    "$brew_command" install goblinreactor/tap/odin-seat </dev/null
    brew_prefix=$("$brew_command" --prefix </dev/null)
    "$brew_prefix/bin/odin-seat" -V </dev/null
    if ! command -v odin-seat >/dev/null 2>&1; then
        printf '\nAdd %s/bin to your PATH to run odin-seat by name.\n' "$brew_prefix"
    fi
}

install_linux() {
    command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1 ||
        die 'Automatic Linux installation needs APT. For other distributions, build from source using the project README.'
    [ -r /etc/os-release ] || die 'Cannot detect this Linux release: /etc/os-release is missing.'
    VERSION_CODENAME= UBUNTU_CODENAME=
    # os-release is supplied by the local OS. Ubuntu derivatives may report
    # their own codename as well as the underlying UBUNTU_CODENAME.
    . /etc/os-release
    suite=${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}
    case "$suite" in
        ''|*[!a-zA-Z0-9._-]*) die 'No usable OS codename was found; see https://apt.goblinreactor.com/ for supported builds.' ;;
    esac
    architecture=$(dpkg --print-architecture)
    case "$architecture" in
        ''|*[!a-zA-Z0-9_-]*) die 'dpkg returned an invalid architecture.' ;;
    esac
    if [ "$(id -u)" != 0 ]; then
        command -v sudo >/dev/null 2>&1 || die 'APT installation needs root privileges. Install sudo or run this installer as root.'
    fi

    say "Checking the Goblin Reactor repository for $suite / $architecture."
    download "https://apt.goblinreactor.com/dists/$suite/InRelease" "$work/InRelease" ||
        die "No repository could be downloaded for $suite. Check the connection and https://apt.goblinreactor.com/ for supported builds."
    download https://apt.goblinreactor.com/goblinreactor-archive-keyring.gpg "$work/keyring.gpg"

    # Bootstrap verification tools from the distribution's existing sources.
    if ! command -v gpg >/dev/null 2>&1 || ! command -v gpgv >/dev/null 2>&1; then
        as_root apt-get update
        as_root apt-get install -y ca-certificates gnupg
    fi
    mkdir -m 0700 "$work/gnupg"
    gpg --batch --no-options --homedir "$work/gnupg" --show-keys --with-colons \
        "$work/keyring.gpg" > "$work/key-info" </dev/null
    # Trust exactly one primary key, with the archive's pinned fingerprint.
    fingerprint=$(awk -F: '
        $1 == "pub" { keys++; primary = 1 }
        $1 == "fpr" && primary { fingerprint = $10; primary = 0 }
        END { if (keys == 1) print fingerprint }
    ' "$work/key-info")
    [ "$fingerprint" = B7B61E8CB85A9270530364F23273CB62402FC793 ] ||
        die 'Unexpected repository signing key; stopping before adding the repository.'
    gpgv --homedir "$work/gnupg" --keyring "$work/keyring.gpg" "$work/InRelease" </dev/null ||
        die 'Repository signature verification failed.'
    # gpgv verifies clearsigned files but does not extract their plaintext.
    # Extract with an isolated gpg keyring after verifying the signature.
    gpg --batch --no-options --homedir "$work/gnupg" --no-default-keyring \
        --keyring "$work/keyring.gpg" --output "$work/Release" \
        --decrypt "$work/InRelease" </dev/null
    published_suite=$(awk '/^Codename:/ { print $2 }' "$work/Release")
    [ "$published_suite" = "$suite" ] || die 'The signed repository codename does not match this OS.'
    architectures=$(awk '/^Architectures:/ { for (i = 2; i <= NF; i++) print $i }' "$work/Release")
    found_architecture=no
    for published_architecture in $architectures; do
        if [ "$published_architecture" = "$architecture" ]; then
            found_architecture=yes
        fi
    done
    [ "$found_architecture" = yes ] ||
        die "The signed repository does not publish $suite / $architecture. See https://apt.goblinreactor.com/ for supported builds."

    printf '%s\n' \
        'Types: deb deb-src' \
        'URIs: https://apt.goblinreactor.com/' \
        "Suites: $suite" \
        'Components: main' \
        "Architectures: $architecture" \
        'Signed-By: /etc/apt/keyrings/goblinreactor-archive-keyring.gpg' \
        > "$work/goblinreactor.sources"
    say 'Adding the verified APT repository and installing Odin Seat.'
    as_root install -d -m 0755 /etc/apt/keyrings /etc/apt/sources.list.d
    as_root install -m 0644 "$work/keyring.gpg" /etc/apt/keyrings/goblinreactor-archive-keyring.gpg
    as_root install -m 0644 "$work/goblinreactor.sources" /etc/apt/sources.list.d/goblinreactor.sources
    as_root apt-get update
    as_root apt-get install -y odin-seat
    /usr/bin/odin-seat -V </dev/null
}

main() {
    case "${1:-}" in
        -h|--help) usage; return ;;
        '') ;;
        *) die "Unknown option: $1 (use --help)." ;;
    esac
    [ "$#" -eq 0 ] || die 'Unexpected arguments (use --help).'
    platform=$(uname -s)
    case "$platform" in
        Darwin|Linux) ;;
        *) die "Unsupported operating system: $platform. Odin Seat supports macOS and Linux." ;;
    esac
    command -v curl >/dev/null 2>&1 || die 'Install curl first, then rerun this installer.'
    work=$(mktemp -d "${TMPDIR:-/tmp}/odin-seat-install.XXXXXXXXXX")
    trap 'rm -rf "$work"' 0
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    case "$platform" in
        Darwin) install_macos ;;
        Linux) install_linux ;;
    esac
    say '' 'Odin Seat is installed. Run odin-seat to start or reattach.' \
        'Tap Alt, release, then ? for help (or use Ctrl-\).'
}

main "$@"
