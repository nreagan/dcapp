#!/usr/bin/env bash
set -e

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="$DCAPP_HOME/pilotlight/out"

if [ $# -lt 2 ]; then
    echo "Usage: dcapp-planet-preprocess <input_dem> <output_dir> [options]"
    echo
    echo "Options:"
    echo "  --radius N"
    echo "  --tile-size N"
    echo "  --min-height N"
    echo "  --max-height N"
    echo "  --meters-per-pixel N"
    echo "  --tree-depth N"
    echo "  --max-base-error N"
    echo "  --prefix NAME"
    echo "  --keep-tiles"
    echo "  -h, --help"
    if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
        exit 0
    fi
    exit 1
fi

INPUT="$1"
OUTPUT="$2"
shift 2

# Get absolute paths
INPUT_ABS="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
OUTPUT_ABS="$(mkdir -p "$OUTPUT" && cd "$OUTPUT" && pwd)"

# Get relative paths using only bash
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

INPUT_REL="$(get_relative_path "$RUN_DIR" "$INPUT_ABS")"
OUTPUT_REL="$(get_relative_path "$RUN_DIR" "$OUTPUT_ABS")"

cd "$RUN_DIR"
cmd="./pilot_light -a dcapp-planet-preprocess $INPUT_REL $OUTPUT_REL $*"
echo "$cmd"
exec $cmd
