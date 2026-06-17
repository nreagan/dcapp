#!/usr/bin/env bash
set -euo pipefail

DCAPP_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DATA_DIR="${DCAPP_PLANET_DATA_DIR:-$DCAPP_HOME/data}"
SOURCE_DIR="$DATA_DIR"
CHUNK_DIR="$DATA_DIR"

IMG_URL="https://imbrium.mit.edu/DATA/LOLA_GDR/POLAR/IMG/LDEM_45S_400M.IMG"
LBL_URL="https://imbrium.mit.edu/DATA/LOLA_GDR/POLAR/IMG/LDEM_45S_400M.LBL"
IMG_FILE="$SOURCE_DIR/LDEM_45S_400M.IMG"
LBL_FILE="$SOURCE_DIR/LDEM_45S_400M.LBL"
PLANET_JSON="$CHUNK_DIR/LDEM_45S_400M.planet.json"
PLANET_TILE_SIZE="${DCAPP_PLANET_TILE_SIZE:-257}"
PLANET_MAX_LOD="${DCAPP_PLANET_MAX_LOD:-5}"

FORCE=false
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            echo "Usage: ./scripts/download-planet-data.sh [--force] [chunkgen options]"
            echo ""
            echo "Downloads the LOLA LDEM_45S_400M lunar DEM and generates planet chunks."
            echo ""
            echo "Environment:"
            echo "  DCAPP_PLANET_DATA_DIR  Override output directory"
            echo "                         default: data"
            echo "  DCAPP_PLANET_TILE_SIZE Override generated chunk tile size"
            echo "                         default: 257"
            echo "  DCAPP_PLANET_MAX_LOD   Override generated max LOD"
            echo "                         default: 5"
            echo ""
            echo "Options:"
            echo "  --force                Regenerate chunks even if the .planet.json exists"
            echo "  -h, --help             Show this help"
            echo ""
            echo "Any other options are passed through to dcapp-planet-chunkgen."
            exit 0
            ;;
        --force)
            FORCE=true
            ;;
        *)
            EXTRA_ARGS+=("$1")
            ;;
    esac
    shift
done

echo "========================================"
echo "Planet Data Download"
echo "========================================"
echo "Data directory: $DATA_DIR"

mkdir -p "$DATA_DIR"

if [ ! -f "$IMG_FILE" ]; then
    echo "Downloading LDEM_45S_400M.IMG..."
    curl -L -o "$IMG_FILE" "$IMG_URL"
else
    echo "LDEM_45S_400M.IMG already downloaded, skipping."
fi

if [ ! -f "$LBL_FILE" ]; then
    echo "Downloading LDEM_45S_400M.LBL..."
    curl -L -o "$LBL_FILE" "$LBL_URL"
else
    echo "LDEM_45S_400M.LBL already downloaded, skipping."
fi

if [ "$FORCE" = true ] || [ ! -f "$PLANET_JSON" ]; then
    echo ""
    echo "Running chunkgen..."
    if [ "$FORCE" = true ]; then
        find "$CHUNK_DIR" -maxdepth 1 -name 'LDEM_45S_400M_f*_l*.p2c' -delete
    fi
    if [ ${#EXTRA_ARGS[@]} -gt 0 ]; then
        "$DCAPP_HOME/bin/dcapp-planet-chunkgen.sh" "$CHUNK_DIR" "$LBL_FILE" --radius 1737400 --prefix LDEM_45S_400M --tile-size "$PLANET_TILE_SIZE" --max-lod "$PLANET_MAX_LOD" "${EXTRA_ARGS[@]}"
    else
        "$DCAPP_HOME/bin/dcapp-planet-chunkgen.sh" "$CHUNK_DIR" "$LBL_FILE" --radius 1737400 --prefix LDEM_45S_400M --tile-size "$PLANET_TILE_SIZE" --max-lod "$PLANET_MAX_LOD"
    fi
else
    echo "LDEM_45S_400M.planet.json already exists, skipping chunkgen. Use --force to regenerate."
fi

echo ""
echo "Planet data ready:"
echo "  $PLANET_JSON"
