#!/usr/bin/env bash
set -e

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="$DCAPP_HOME/pilotlight/out"

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    echo "Usage: dcapp-planet-backfill <planet_json> [output_dcpm]"
    exit 0
fi

if [ $# -lt 1 ]; then
    echo "Usage: dcapp-planet-backfill <planet_json> [output_dcpm]"
    exit 1
fi

INPUT="$1"
shift

get_relative_path() {
    local source="$1"
    local target="$2"

    local common_part="$source"
    local result=""

    while [[ "${target#"$common_part"}" == "${target}" ]]; do
        common_part="$(dirname "$common_part")"
        result="../$result"
    done

    if [[ "$common_part" == "/" ]]; then
        result="$result${target:1}"
    else
        result="$result${target#"$common_part"/}"
    fi

    echo "$result"
}

INPUT_ABS="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
INPUT_REL="$(get_relative_path "$RUN_DIR" "$INPUT_ABS")"

ARGS=("$INPUT_REL")
if [ $# -gt 0 ] && [[ "${1:-}" != -* ]]; then
    OUTPUT="$1"
    shift
    mkdir -p "$(dirname "$OUTPUT")"
    OUTPUT_ABS="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
    ARGS+=("$(get_relative_path "$RUN_DIR" "$OUTPUT_ABS")")
fi

if [ $# -gt 0 ]; then
    echo "Error: unexpected argument: $1" >&2
    exit 1
fi

cd "$RUN_DIR"
cmd=("./pilot_light" "-a" "dcapp-planet-backfill" "${ARGS[@]}")
printf '%q ' "${cmd[@]}"
printf '\n'
exec "${cmd[@]}"
