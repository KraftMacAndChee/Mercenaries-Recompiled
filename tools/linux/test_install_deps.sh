#!/usr/bin/env bash
# Self-check for install_deps.sh distro detection.
#   bash tools/linux/test_install_deps.sh
set -uo pipefail

here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0

# Pull detect_family out of the script without running the install half.
eval "$(sed -n '/^detect_family()/,/^}/p' "$here/install_deps.sh")"

check() { # name, os-release body, expected
    printf '%s\n' "$2" > "$tmp/os-release"
    got=$(OS_RELEASE="$tmp/os-release" detect_family 2>/dev/null) || got="(none)"
    if [ "$got" = "$3" ]; then
        echo "ok   $1 -> $got"
    else
        echo "FAIL $1 -> got '$got', want '$3'"; fails=$((fails + 1))
    fi
}

# Real ID/ID_LIKE lines from each distro.
check cachyos    'ID=cachyos
ID_LIKE=arch'                            arch
check arch       'ID=arch'               arch
check manjaro    'ID=manjaro
ID_LIKE=arch'                            arch
check ubuntu     'ID=ubuntu
ID_LIKE=debian'                          debian
check debian     'ID=debian'             debian
check fedora     'ID=fedora'             fedora
check rhel       'ID=rhel
ID_LIKE="fedora"'                        fedora
check unknown    'ID=plan9'              "(none)"

# Missing file must fail cleanly, not crash.
got=$(OS_RELEASE="$tmp/nope" detect_family 2>/dev/null) || got="(none)"
[ "$got" = "(none)" ] && echo "ok   missing os-release -> (none)" \
    || { echo "FAIL missing os-release -> '$got'"; fails=$((fails + 1)); }

echo
[ "$fails" -eq 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
