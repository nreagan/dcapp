#!/usr/bin/env bash
set -e

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="$DCAPP_HOME/pilotlight/out"

if [ $# -lt 1 ]; then
    cd "$RUN_DIR"
    exec ./pilot_light -a dcapp-planet-validate --planet-validate-help
fi

to_abs_existing() {
    local path="$1"
    if [[ "$path" == /* ]]; then
        echo "$path"
    else
        echo "$(cd "$(dirname "$path")" && pwd)/$(basename "$path")"
    fi
}

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            ARGS+=("--planet-validate-help")
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
cmd=("./pilot_light" "-a" "dcapp-planet-validate" "${ARGS[@]}")
echo "${cmd[*]}"
exec "${cmd[@]}"
