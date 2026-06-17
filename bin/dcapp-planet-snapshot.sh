#!/usr/bin/env bash
set -e

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="$DCAPP_HOME/pilotlight/out"

if [ $# -lt 1 ]; then
    cd "$RUN_DIR"
    exec ./pilot_light -a dcapp-planet-snapshot --planet-snapshot-help
fi

to_abs_existing() {
    local path="$1"
    if [[ "$path" == /* ]]; then
        echo "$path"
    else
        echo "$(cd "$(dirname "$path")" && pwd)/$(basename "$path")"
    fi
}

to_abs_output() {
    local path="$1"
    local dir
    dir="$(dirname "$path")"
    mkdir -p "$dir"
    if [[ "$path" == /* ]]; then
        echo "$path"
    else
        echo "$(cd "$dir" && pwd)/$(basename "$path")"
    fi
}

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            ARGS+=("--planet-snapshot-help")
            ;;
        --output)
            key="$1"
            shift
            ARGS+=("$key" "$(to_abs_output "$1")")
            ;;
        --vertex-shader|--fragment-shader)
            key="$1"
            shift
            if [[ -e "$1" ]]; then
                ARGS+=("$key" "$(to_abs_existing "$1")")
            else
                ARGS+=("$key" "$1")
            fi
            ;;
        --*)
            ARGS+=("$1")
            ;;
        *)
            if [[ -e "$1" ]]; then
                ARGS+=("$(to_abs_existing "$1")")
            else
                ARGS+=("$1")
            fi
            ;;
    esac
    shift
done

cd "$RUN_DIR"
cmd=("./pilot_light" "-a" "dcapp-planet-snapshot" "${ARGS[@]}")
echo "${cmd[*]}"
exec "${cmd[@]}"
