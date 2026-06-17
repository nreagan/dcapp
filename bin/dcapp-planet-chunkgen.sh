#!/usr/bin/env bash
set -e

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="$DCAPP_HOME/pilotlight/out"

if [ $# -lt 2 ]; then
    cd "$RUN_DIR"
    exec ./pilot_light -a dcapp-planet-chunkgen --planet-help
fi

to_abs_output_dir() {
    local path="$1"
    mkdir -p "$path"
    if [[ "$path" == /* ]]; then
        echo "$(cd "$path" && pwd)"
    else
        echo "$(cd "$path" && pwd)"
    fi
}

source_spec_to_abs() {
    local spec="$1"
    local path="$spec"
    local suffix=""

    if [[ "$spec" =~ ^(.+),([+-]?[0-9]+)$ ]]; then
        path="${BASH_REMATCH[1]}"
        suffix=",${BASH_REMATCH[2]}"
    fi

    if [[ -e "$path" ]]; then
        local abs
        if [[ "$path" == /* ]]; then
            abs="$path"
        else
            abs="$(cd "$(dirname "$path")" && pwd)/$(basename "$path")"
        fi
        echo "$abs$suffix"
    else
        echo "$spec"
    fi
}

OUTPUT_ABS="$(to_abs_output_dir "$1")"
shift

ARGS=("$OUTPUT_ABS")
while [ $# -gt 0 ]; do
    case "$1" in
        --source)
            key="$1"
            shift
            ARGS+=("$key" "$(source_spec_to_abs "$1")")
            ;;
        --radius|--tile-size|--max-lod|--prefix)
            key="$1"
            shift
            ARGS+=("$key" "$1")
            ;;
        *)
            if [[ "$1" == -* ]]; then
                ARGS+=("$1")
            else
                ARGS+=("$(source_spec_to_abs "$1")")
            fi
            ;;
    esac
    shift
done

cd "$RUN_DIR"
cmd=("./pilot_light" "-a" "dcapp-planet-chunkgen" "${ARGS[@]}")
echo "${cmd[*]}"
exec "${cmd[@]}"
