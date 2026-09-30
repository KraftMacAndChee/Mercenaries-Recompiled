#!/usr/bin/env bash
# install_deps.sh - Install build dependencies for the native Linux build of
# the xboxrecomp runtime libraries.
#
# Detects the host family from /etc/os-release and uses its package manager:
#   Debian/Ubuntu (incl. WSL) -> apt
#   Arch/CachyOS/Manjaro      -> pacman
#   Fedora/RHEL               -> dnf
#
#   bash tools/linux/install_deps.sh
#
# Requires sudo (will prompt for a password).
set -euo pipefail

# Overridable so the self-check below can feed it fake os-release files.
OS_RELEASE="${OS_RELEASE:-/etc/os-release}"

# Same dependencies throughout, spelled per distro:
#   build toolchain + cmake + pkg-config
#   SDL2    - window, input, audio
#   OpenGL headers + libepoxy - GL loader for the d3d8 backend
#   OpenSSL - crypto, replaces bcrypt
#   ffmpeg  - video decode/demux/scale
detect_family() {
    local id id_like
    [ -r "$OS_RELEASE" ] || return 1
    id=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID:-}")
    id_like=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID_LIKE:-}")

    case " $id $id_like " in
        *" debian "*|*" ubuntu "*) echo debian ;;
        *" arch "*|*" archlinux "*) echo arch ;;
        *" fedora "*|*" rhel "*|*" centos "*) echo fedora ;;
        *) return 1 ;;
    esac
}

family=$(detect_family) || {
    echo "Could not identify the distro from $OS_RELEASE." >&2
    echo "Install these by hand: a C toolchain, cmake, pkg-config, SDL2," >&2
    echo "OpenGL headers, libepoxy, OpenSSL, and ffmpeg (all with headers)." >&2
    exit 1
}

case "$family" in
    debian)
        PKGS=(cmake build-essential pkg-config libsdl2-dev libgl1-mesa-dev
              libepoxy-dev libssl-dev libavcodec-dev libavformat-dev
              libavutil-dev libswscale-dev)
        echo "Detected Debian/Ubuntu. Installing ${#PKGS[@]} packages..."
        sudo apt-get update
        sudo apt-get install -y "${PKGS[@]}"
        ;;
    arch)
        # base-devel covers gcc/make/pkgconf; ffmpeg and mesa ship headers.
        # sdl2 resolves to sdl2-compat on current Arch.
        PKGS=(base-devel cmake sdl2 mesa libepoxy openssl ffmpeg)
        echo "Detected Arch. Installing ${#PKGS[@]} packages..."
        sudo pacman -S --needed "${PKGS[@]}"
        ;;
    fedora)
        PKGS=(cmake gcc make pkgconf-pkg-config SDL2-devel mesa-libGL-devel
              libepoxy-devel openssl-devel ffmpeg-devel)
        echo "Detected Fedora/RHEL. Installing ${#PKGS[@]} packages..."
        echo "(ffmpeg-devel needs RPM Fusion enabled.)"
        sudo dnf install -y "${PKGS[@]}"
        ;;
esac

echo
echo "Done. Build deps installed."
