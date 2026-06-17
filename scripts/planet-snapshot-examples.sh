#!/usr/bin/env bash
set -euo pipefail

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SNAPSHOT="$DCAPP_HOME/bin/dcapp-planet-snapshot.sh"
PLANET_DATA="$DCAPP_HOME/data/LDEM_45S_400M.planet.json"
OUT_DIR="$DCAPP_HOME/data"
EXAMPLE="${1:-}"

if [[ "$EXAMPLE" != "1" && "$EXAMPLE" != "2" && "$EXAMPLE" != "3" ]]; then
    echo "Usage: $0 1|2|3"
    echo "  1  default manifest view"
    echo "  2  tile debug view"
    echo "  3  oblique Cartesian view"
    exit 1
fi

if [[ ! -f "$PLANET_DATA" ]]; then
    echo "Missing planet data: $PLANET_DATA"
    echo "Run ./scripts/download-planet-data.sh first, then rebuild if needed."
    exit 1
fi

mkdir -p "$OUT_DIR"

case "$EXAMPLE" in
    1)
        "$SNAPSHOT" \
            "$PLANET_DATA" \
            --width 1280 --height 720 \
            --fov 60 \
            --output "$OUT_DIR/planet-default.png"
        ;;
    2)
        "$SNAPSHOT" \
            "$PLANET_DATA" \
            --width 1024 --height 1024 \
            --fov 55 \
            --show-tiles \
            --output "$OUT_DIR/planet-tiles.png"
        ;;
    3)
        "$SNAPSHOT" \
            "$PLANET_DATA" \
            --eye -494826 -3190740 1882148 \
            --target 0 0 0 \
            --width 1280 --height 720 \
            --fov 60 \
            --output "$OUT_DIR/planet-oblique.png"
        ;;
esac
