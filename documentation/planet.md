# Planet Terrain Architecture

Planet is the full-planet terrain path. It uses canonical cube-sphere chunks in
Pilotlight's internal Cartesian frame; runtime rendering does not consume
projected-grid chunks.

## Core Model

Planet uses a global cube-sphere tile pyramid:

```text
face / lod / x / y
```

Each face is a quadtree. Runtime selection asks for a desired tile address and
falls back to the nearest available parent when sparse high-resolution data is
missing.

## Data Flow

The intended generation pipeline is:

```text
DEM sources, each in its own CRS
  -> source catalog with footprint, priority, resolution, nodata policy
  -> cube-sphere output tiles
  -> per-sample source selection and inverse projection
  -> geodetic lat/lon
  -> Pilotlight Cartesian vertices
  -> .planet manifest and chunk payloads
```

Source CRS math belongs in chunk generation. Runtime chunks should already be in
Pilotlight Cartesian coordinates.

The current generator supports GDAL-readable polar stereographic DEMs plus
cylindrical/equirectangular sources, including projected simple-cylindrical
rasters and angular lon/lat rasters. Additional CRSs should be added as
chunkgen source adapters that emit the same cube-sphere tile payloads.

## Source Priority

Overlapping DEMs are resolved by source metadata:

- Valid data beats missing/nodata.
- Higher explicit priority wins.
- If priorities tie, lower native meters-per-pixel wins.
- Parent fallback handles sparse high-resolution tile coverage at runtime.

## Current Extension Scope

`pl_planet_ext` currently provides the canonical contract layer:

- DEM source catalog records.
- Sparse cube-sphere tile manifest records.
- Exact tile lookup.
- Parent fallback selection.
- Source selection by footprint, priority, and native resolution.
- Cube face `u/v` to direction helpers.
- `.planet.json` manifest parsing.
- `.p2c` chunk header/payload loading.
- A GPU renderer that keeps a bounded resident tile cache and renders selected
  tiles to an offscreen view.
- View-dependent culling, screen-space LOD selection, and parent fallback for
  sparse high-resolution coverage.
- Runtime payload streaming from `.p2c` files into resident GPU cache slots.

Threaded/asynchronous prefetch can build against this contract without adding
another coordinate frame to the runtime path.

## Current Chunkgen Bootstrap

`dcapp-planet-chunkgen` is the first executable slice of the pipeline. It uses
GDAL to read one or more DEMs, builds the source catalog, resolves source
priority, writes `.p2c` cube-sphere tile payloads, and writes a `.planet.json`
manifest:

```bash
bin/dcapp-planet-chunkgen.sh /tmp/moon2 data/LDEM_45S_400M.LBL data/LDEM_45S_100M.LBL --max-lod 4 --radius 1737400
```

Sources can include an explicit priority suffix:

```bash
bin/dcapp-planet-chunkgen.sh /tmp/moon2 data/LDEM_45S_400M.LBL,10 data/LDEM_45S_100M.LBL,0
```

Use `--manifest-only` for a cheap source-selection pass without payloads.

Payload chunks contain a fixed grid mesh for each cube-sphere tile:

- split-double Cartesian positions in Pilotlight's internal frame
- radial normals and UVs
- 32-bit index data
- tile bounds for runtime culling and streaming

The generator chooses a primary source per tile for provenance, but samples
sources per vertex so DEM boundaries can cut through a tile without producing
overlapping chunks. `--height-mode auto` treats LOLA-style radius products as
radial distances and smaller elevation products as offsets from the manifest
radius; `radial` and `offset` can force either interpretation.

The runtime layer can parse `.planet.json`, keep all tile headers/provenance as
CPU metadata, stream selected `.p2c` payloads into a bounded GPU cache, and
drive view-dependent cube-sphere LOD selection with parent fallback.

## Runtime Loading and Rendering

`pl_planet_ext` can load a generated manifest at runtime:

- `load_manifest_json(path, validatePayloads)`
- `resolve_tile_path(manifest, tile, ...)`
- `read_chunk_header(path, ...)`
- `read_chunk_payload(path, header, vertices, indices)`
- `create_planet(cmd, init)` for uploading manifest payloads to GPU buffers
- `create_view(planet, cmd, init)` / `render_view(view, camera, cmd)` for
  offscreen rendering
- `set_view_runtime_options(view, options)` to control debug flags and the
  screen-space LOD threshold

The validation app exercises that route:

```bash
bin/dcapp-planet-validate.sh /tmp/moon2/planet.planet.json --read-first
```

The GPU smoke/snapshot app exercises manifest loading, chunk upload, planet
shader compilation, rendering, and readback:

```bash
bin/dcapp-planet-snapshot.sh /tmp/moon2/planet.planet.json --output /tmp/moon2.png --show-tiles
```

Use `--lod-threshold N` on the snapshot app to make the selector more or less
aggressive. Lower values refine farther; higher values stay on coarser parents.
Use `--gpu-cache-mb N` to force a smaller or larger resident cache during GPU
smoke tests.
