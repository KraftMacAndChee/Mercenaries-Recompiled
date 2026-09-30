#!/usr/bin/env bash
# Steam launch option: bash /path/to/trace-proton-performance.sh %command%
set -euo pipefail

output="${PWD}/proton-traces/$(date +%Y%m%d-%H%M%S)-$$"
if [[ "${1:-}" == "--output" ]]; then
    [[ $# -ge 3 ]] || { echo "Usage: $0 [--output directory] command [args...]" >&2; exit 2; }
    output="$2"
    shift 2
fi
[[ $# -gt 0 ]] || { echo "Usage: $0 [--output directory] command [args...]" >&2; exit 2; }
mkdir -p -- "$output"
output="$(cd -- "$output" && pwd -P)"
printf 'Recording Proton performance to %s\n' "$output"
{
    date -u +%FT%TZ
    uname -srmo
    if [[ -r /etc/os-release ]]; then cat /etc/os-release; fi
    if command -v vulkaninfo >/dev/null && command -v timeout >/dev/null; then
        timeout 15s vulkaninfo --summary 2>&1 || true
    else
        echo 'vulkaninfo unavailable; DXVK logs may identify the active GPU.'
    fi
} > "$output/system.txt"

# Proton exposes the Linux root as Z:. Keep preview logs with the host trace.
preview="Z:${output//\//\\}\\preview-logs"
export PROTON_LOG=1 PROTON_LOG_DIR="$output"
export DXVK_LOG_LEVEL=info DXVK_LOG_PATH="$output"
export MERCENARIES_PREVIEW_LOG_DIR="$preview"
export MERCENARIES_TRACE_PRESENT_TIMING=1
export MERCENARIES_TRACE_D3D_DRAW_PERF=1
export MERCENARIES_TRACE_TEXTURE_PERF=1
export MERCENARIES_TRACE_SHADER_TIMING=1
export MERCENARIES_DIAGNOSTIC_BUFFERED_LOGS=1
exec "$@" > "$output/launcher.log" 2>&1
