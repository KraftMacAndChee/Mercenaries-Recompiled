#!/usr/bin/env bash
#
# Build-Mercenaries-Linux.sh
#
# Rebuild the Mercenaries recompiled port for Windows x64 from Linux using
# LLVM-MinGW, starting from an already-extracted retail game tree (produced by
# extract-xiso or the bundled xdvdfs). This mirrors Build-From-Iso.ps1, which
# only runs on Windows with Visual Studio 2022.
#
# Usage:
#   LLVM_MINGW=/path/to/llvm-mingw \
#     ./scripts/Build-Mercenaries-Linux.sh --game-dir "/path/to/Mercenaries (USA, Europe).xiso"
#
# The extracted directory must contain default.xbe with SHA-256
# AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7.
#
# Output: <repo>/build/mercenaries-mingw/bin/mercenaries_recomp.exe
#         <repo>/build/mercenaries-mingw/bin/Mercenaries Recompiled.exe
#
# Run the result with Wine, pointing the runtime directly at the game tree:
#   cd <repo>/build/mercenaries-mingw/bin
#   MERCENARIES_GAME_DIR='Z:\path\to\extracted\tree' wine mercenaries_recomp.exe
#
# NOTE: The bundled DSP56300 backend is an MSVC-ABI Rust archive and is not
# linked by this cross build; the portable apu_dsp.c fallback is used instead.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PORT_ROOT/../.." && pwd)"

GAME_DIR=""
BUILD_DIR="$REPO_ROOT/build/mercenaries-mingw"
BUILD_TYPE="Release"
MAX_PASSES=32

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-dir)  GAME_DIR="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --debug)     BUILD_TYPE="Debug"; shift ;;
        -h|--help)   sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$GAME_DIR" ]]; then
    echo "error: --game-dir is required" >&2
    exit 2
fi
GAME_DIR="$(cd "$GAME_DIR" && pwd)"
XBE="$GAME_DIR/default.xbe"
if [[ ! -f "$XBE" ]]; then
    echo "error: default.xbe not found in $GAME_DIR" >&2
    exit 1
fi

EXPECTED_XBE="aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7"
ACTUAL_XBE="$(sha256sum "$XBE" | awk '{print $1}')"
if [[ "$ACTUAL_XBE" != "$EXPECTED_XBE" ]]; then
    echo "error: unsupported default.xbe (got $ACTUAL_XBE)" >&2
    exit 1
fi

if [[ -z "${LLVM_MINGW:-}" ]]; then
    echo "error: set LLVM_MINGW to the llvm-mingw root directory" >&2
    exit 1
fi

PY="$REPO_ROOT/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
    echo "==> Creating Python virtual environment"
    python3 -m venv "$REPO_ROOT/.venv"
    "$PY" -m pip install --disable-pip-version-check --upgrade pip
fi
"$PY" -m pip install --disable-pip-version-check "capstone==5.0.6"

ANALYSIS="$PORT_ROOT/analysis"
DISASM="$ANALYSIS/disasm"
FUNCID="$ANALYSIS/func_id"
RECOMP="$ANALYSIS/recomp"
XBE_JSON="$ANALYSIS/xbe.json"
GEN="$PORT_ROOT/src/recomp/gen"
MANUAL="$PORT_ROOT/manual-seeds.json"
MERGED="$ANALYSIS/seeds.json"
MERGE="$SCRIPT_DIR/merge-function-seeds.py"

cd "$REPO_ROOT"
mkdir -p "$ANALYSIS" "$GEN"

echo "==> Parsing XBE metadata"
"$PY" -m tools.xbe_parser "$XBE" --json "$XBE_JSON" --quiet

seed="$MANUAL"
converged=0
for ((pass = 1; pass <= MAX_PASSES; pass++)); do
    echo "==> Analysis pass $pass of $MAX_PASSES"
    input_hash="$(sha256sum "$seed" | awk '{print $1}')"

    "$PY" -m tools.disasm "$XBE" \
        -o "$DISASM" \
        --analysis-json "$XBE_JSON" \
        --extra-sections "XMV,XACTENG,D3D,XGRPH,DSOUND,WMADEC,XPP,DOLBY" \
        --seed-functions "$seed" \
        --protected-seed-functions "$MANUAL" \
        --force >/dev/null

    "$PY" -m tools.func_id "$XBE" \
        --functions "$DISASM/functions.json" \
        --strings "$DISASM/strings.json" \
        --xrefs "$DISASM/xrefs.json" \
        -o "$FUNCID" >/dev/null

    "$PY" "$MERGE" \
        --functions "$DISASM/functions.json" \
        --identified "$FUNCID/identified_functions.json" \
        --manual-seeds "$MANUAL" \
        --output "$MERGED"

    output_hash="$(sha256sum "$MERGED" | awk '{print $1}')"
    if [[ "$output_hash" == "$input_hash" ]]; then
        converged=1
        break
    fi
    seed="$MERGED"
done
[[ "$converged" -eq 1 ]] || echo "warning: function discovery did not converge" >&2

echo "==> Lifting functions to generated C"
"$PY" -m tools.recomp "$XBE" \
    --all --split 1000 \
    --function-range 0x00110D00:0x00110D5B \
    --function-range 0x0014FF00:0x0015009E \
    --function-range 0x001B10B0:0x001B169F \
    --function-range 0x001B1BA0:0x001B27E8 \
    --function-range 0x001B9EB0:0x001BAC1A \
    --function-range 0x000658B0:0x00065AA5 \
    --function-range 0x0008DB70:0x0008DC9D \
    --function-range 0x000CB120:0x000CB4DC \
    --function-range 0x000DFF60:0x000E0097 \
    --function-range 0x0014F720:0x0014FEDE \
    --function-range 0x0018D2D0:0x0018D45E \
    --function-range 0x001DAFA0:0x001DB0C5 \
    --function-range 0x001DCF00:0x001DCF40 \
    --function-range 0x001C53F0:0x001C5999 \
    --function-range 0x002318A2:0x00231B06 \
    --function-range 0x002965F0:0x00296B47 \
    --function-range 0x000330A0:0x00033457 \
    --function-range 0x00108A00:0x00108E7D \
    --function-range 0x00145460:0x001455D2 \
    --function-range 0x00162610:0x00162652 \
    --function-range 0x00167E40:0x00168016 \
    --gen-dir "$GEN" \
    --manual-function 0x000112B0 \
    --manual-function 0x00178870 \
    --manual-function 0x001E26E0 \
    --manual-function 0x001E89E0 \
    --manual-function 0x001E91ED \
    --manual-function 0x001F9D70 \
    --manual-function 0x001F9DA0 \
    --manual-function 0x001F9DE0 \
    --manual-function 0x0022B150 \
    --manual-function 0x0022B178 \
    --manual-function 0x0022DDA4 \
    --manual-function 0x0022E51F \
    --manual-function 0x0022E713 \
    --manual-function 0x0022F49E \
    --manual-function 0x0022F51C \
    --manual-function 0x0022F5A7 \
    --manual-function 0x0022F6A5 \
    --manual-function 0x0022F7C6 \
    --manual-function 0x0022F9BC \
    --manual-function 0x0022FA9E \
    --disasm-dir "$DISASM" \
    --func-id-dir "$FUNCID" \
    --manual-function 0x0023BE3C \
    --manual-function 0x002A6713 \
    -o "$RECOMP"

echo "==> Patching generated C"
"$PY" "$PORT_ROOT/scripts/Patch-Generated.py" "$GEN"

"$PY" -c "import json;s=json.load(open('$RECOMP/summary.json'));print('Lift: {}/{} functions, {} failures, {} unresolved.'.format(s['translated'],s['total'],s['failed'],s['unresolved_stubs']))"

echo "==> Configuring (LLVM-MinGW x86_64-w64-mingw32)"
export LLVM_MINGW
cmake -S "$PORT_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/linux-mingw-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "==> Building"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "Built: $BUILD_DIR/bin/mercenaries_recomp.exe"
echo "Launcher: $BUILD_DIR/bin/Mercenaries Recompiled.exe"
echo
echo "Supersampling (SSAA) is selectable in the in-game Recomp Options menu or"
echo 'via mercenaries_recomp.ini: [RecompOptions] Supersampling=0..3 (off/1.5x/2x/3x).'
