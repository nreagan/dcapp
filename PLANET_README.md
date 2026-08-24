# Planet system: internal developer and owner guide

This document explains the **implementation** of dcapp's planet system. It is
for the developer who owns the processor, renderer, shaders, dcapp adapter, and
their data formats. It is deliberately separate from the end-user Planet API
documentation.

The descriptions and equations below follow the code as it exists today. When
the implementation differs from a textbook algorithm, the current behavior is
called out explicitly. Sections headed **Owner notes** identify assumptions,
risks, or technical debt that should be understood before changing the system.

## Contents

1. [System at a glance](#1-system-at-a-glance)
2. [Source map and ownership boundaries](#2-source-map-and-ownership-boundaries)
3. [Terminology and invariants](#3-terminology-and-invariants)
4. [Build products and dependencies](#4-build-products-and-dependencies)
5. [End-to-end data flow](#5-end-to-end-data-flow)
6. [Offline stage: GDAL and `dcapp-planet-chunkgen`](#6-offline-stage-gdal-and-dcapp-planet-chunkgen)
7. [Offline stage: terrain processor internals](#7-offline-stage-terrain-processor-internals)
8. [The `.planet.json` and `.chu` formats](#8-the-planetjson-and-chu-formats)
9. [Runtime metadata loading and planet creation](#9-runtime-metadata-loading-and-planet-creation)
10. [Runtime streaming, request scheduling, and eviction](#10-runtime-streaming-request-scheduling-and-eviction)
11. [Visibility and CDLOD selection math](#11-visibility-and-cdlod-selection-math)
12. [Runtime overlay texture slicing](#12-runtime-overlay-texture-slicing)
13. [GPU architecture](#13-gpu-architecture)
14. [Vertex shader, large-world precision, and flattening](#14-vertex-shader-large-world-precision-and-flattening)
15. [Fragment shader, lighting, and texture composition](#15-fragment-shader-lighting-and-texture-composition)
16. [How dcapp owns and drives the extension](#16-how-dcapp-owns-and-drives-the-extension)
17. [Camera and coordinate-frame math](#17-camera-and-coordinate-frame-math)
18. [Planet annotations, local geometry, GeoJSON, and breadcrumbs](#18-planet-annotations-local-geometry-geojson-and-breadcrumbs)
19. [Snapshot renderer](#19-snapshot-renderer)
20. [Memory, performance, and latency model](#20-memory-performance-and-latency-model)
21. [Failure modes and current owner audit](#21-failure-modes-and-current-owner-audit)
22. [Safe modification recipes](#22-safe-modification-recipes)
23. [Debugging and validation playbook](#23-debugging-and-validation-playbook)
24. [Maintainer checklist](#24-maintainer-checklist)

## 1. System at a glance

The planet feature is not one extension in isolation. It is a pipeline with an
offline half and a runtime half:

```text
projected GeoTIFF DEM
        |
        | GDAL metadata, pixels, scale, NoData, geotransform
        v
dcapp-planet-chunkgen
        |
        | 16-bit temporary height PNGs + projection/tile metadata
        v
pl_planet_processor_ext
        |
        | projection to a sphere, normals, BTT meshes, CDLOD quadtree
        v
  .planet.json + one .chu per raster tile
        |
        | metadata parse + .chu topology scan
        v
src/app/planet.c ---- dcapp display model/runtime or logic API
        |
        | one shared plPlanet, zero or more plPlanetView objects
        v
pl_planet_ext
        |
        | CPU visibility/LOD, one-chunk-per-frame streaming, texture slicing
        v
pooled GPU vertex/index buffers + bindless overlay textures
        |
        | camera-relative high/low vertex shader, reverse-Z render pass
        v
offscreen RGBA planet view ---- dc_draw composites it into the display
```

The main separation of responsibility is:

- **GDAL frontend:** understands the source raster and its spatial reference.
- **Processor extension:** converts a regular projected heightfield into
  sphere-space, crack-constrained triangle meshes and a quadtree file.
- **Runtime extension:** owns GPU caches, streams chunk payloads, chooses LOD,
  renders an offscreen view, and places up to five runtime textures.
- **dcapp adapter:** parses metadata, exposes stable application handles,
  translates coordinate systems, creates cameras, queues annotations, and
  integrates the offscreen render into dcapp's frame.
- **Display builder/runtime:** converts XML definitions into the same adapter
  objects and updates them from variables each frame.
- **Logic API:** lets a loaded logic module create and draw the same objects
  programmatically.

Two lifetime rules are fundamental:

1. A `plPlanet` owns terrain/cache data and can be shared by many views.
2. A `plPlanetView` owns a render target, depth target, pipelines, and overlay
   draw list, but points back to the shared planet.

Views must be destroyed before their planet. Planets must be destroyed before
the planet extension is cleaned up. dcapp follows that order.

## 2. Source map and ownership boundaries

### Core extension

| File | Responsibility |
| --- | --- |
| [`extensions/pl_planet_ext.h`](extensions/pl_planet_ext.h) | Runtime extension API, flags, options, and statistics. Current API version is `0.11.0`. |
| [`extensions/pl_planet_ext.c`](extensions/pl_planet_ext.c) | GPU initialization, planet/view creation, streaming, LRU cache, LOD traversal, culling, texture slicing, and rendering. |
| [`extensions/pl_planet_processor_ext.h`](extensions/pl_planet_processor_ext.h) | Processor API and the shared process/chunk structures. Current API version is `0.4.0`. |
| [`extensions/pl_planet_processor_ext.c`](extensions/pl_planet_processor_ext.c) | Heightmap stitching, projection, error preprocessing, BTT mesh generation, `.chu` writer, and `.chu` metadata loader. |

### GPU contract

| File | Responsibility |
| --- | --- |
| [`shaders/pl_shader_interop_planet.h`](shaders/pl_shader_interop_planet.h) | C/GLSL-compatible per-draw data and the 4096-texture bindless limit. |
| [`shaders/planet.vert`](shaders/planet.vert) | Vertex layouts, high/low reconstruction, compensated camera subtraction, flattening, normal decode, and debug colors. |
| [`shaders/planet.frag`](shaders/planet.frag) | Lambert lighting, five overlay samples, and debug display modes. |

### dcapp integration

| File | Responsibility |
| --- | --- |
| [`src/app/planet.c`](src/app/planet.c) | Stable dcapp handles, JSON parse, extension initialization, projection conversions, view/camera creation, shaders, textures, GeoJSON, and breadcrumbs. |
| [`src/app/planet.h`](src/app/planet.h) | Internal adapter interface used by dcapp subsystems. |
| [`src/app/planet_api.h`](src/app/planet_api.h) | Logic-facing planet API table. This is an adapter API, not the low-level extension API. |
| [`src/app/planet_types.h`](src/app/planet_types.h) | Public dcapp planet enums, handles, flags, and compact types. |
| [`src/app/draw.c`](src/app/draw.c) | Queues a planet view into dc_draw and translates all planet annotation calls. |
| [`src/app/draw_api.h`](src/app/draw_api.h) | Logic-facing draw API for planet views and annotations. |
| [`src/app/xml.c`](src/app/xml.c) | Parses `<Planet>`, data, texture, shader, view, and annotation XML nodes. |
| [`src/app/scene.c`](src/app/scene.c) | Stores declarative planet definitions and view-node references in the sealed scene. |
| [`src/app/renderer.c`](src/app/renderer.c) | Creates scene-owned planets/views, updates variables/textures, queues views, and flushes planet rendering. |
| [`src/app/node.h`](src/app/node.h) | Planet-related node payloads and runtime state. |
| [`apps/dcapp.c`](apps/dcapp.c) | Loads extensions, creates contexts, exposes API tables, and defines frame/shutdown order. |

### Tools and examples

| File | Responsibility |
| --- | --- |
| [`apps/dcapp_planet_chunkgen.c`](apps/dcapp_planet_chunkgen.c) | GDAL-based GeoTIFF frontend and command-line application. |
| [`apps/dcapp_planet_snapshot.c`](apps/dcapp_planet_snapshot.c) | Headless-ish settling renderer and GPU readback to PNG. |
| [`bin/dcapp-planet-chunkgen.sh`](bin/dcapp-planet-chunkgen.sh) | Normalizes paths and launches chunkgen through `pilot_light`. |
| [`bin/dcapp-planet-snapshot.sh`](bin/dcapp-planet-snapshot.sh) | Normalizes snapshot paths and launches the snapshot application. |
| [`samples/planet/planet.xml`](samples/planet/planet.xml) | Full declarative integration example. |
| [`samples/planet/logic/logic.c`](samples/planet/logic/logic.c) | Programmatic planet, view, textures, shader changes, and annotations. |
| [`src/geo.c`](src/geo.c) | Geodetic/cartesian, polar stereographic, NED, and rotation utilities used by the adapter. |

The extension itself does **not** know XML, dcapp variables, stable IDs,
geodetic API conventions, or application layout. Conversely, the display
runtime does not generate meshes or make LOD decisions. Keep this boundary when
adding features.

## 3. Terminology and invariants

### Raster tile versus quadtree chunk

A **tile** is one square source-height region and corresponds to one `.chu`
file. A **chunk** is one node of the quadtree inside that tile. One planet may
contain many tiles, and each tile contains

\[
C(D) = \sum_{i=0}^{D-1}4^i = \frac{4^D-1}{3}
\]

chunks for tree depth \(D\). At the default depth 6, each tile contains 1,365
chunks.

The root has `uLevel = D - 1`; each child decrements the value; leaves have
level 0. This is the opposite of conventions where level 0 means root. Every
formula involving `uLevel` must respect the implementation's convention.

### Requested tile size versus effective processor grid

`uSize` is described and emitted as a tile size in pixels. The processor needs
a grid with \(2^L+1\) vertices so its square has exactly \(2^L\) intervals. It
computes

\[
L = \operatorname{round}(\log_2(uSize-1))
\]

and increments \(L\) until \(2^L+1 \ge uSize\), then uses

\[
N = 2^L+1.
\]

For the default `uSize = 4096`, the effective grid is 4097 by 4097 with 4096
intervals. The 4096 by 4096 intermediate PNG supplies most vertices; east and
south boundary samples are copied from neighboring tiles. This distinction is
essential to understanding seams and pixel-center math.

### Coordinate conventions

- Units are meters unless explicitly stated.
- Geodetic latitude/longitude passed through dcapp are degrees.
- Trigonometric formulas operate in radians after conversion.
- Cartesian axes are `+Y` north pole, longitude zero on `+Z`, and positive
  longitude toward `+X`.
- Raster X increases right/east. Raster row increases down/south.
- Projected coordinates use conventional easting X and northing Y.
- The processor's temporary local grid uses X and Z; it negates raster-local Z
  to obtain projected northing.
- Runtime rendering is camera-relative even though files contain planet-scale
  Cartesian coordinates.

### Current hard limits

- Five overlay texture slots per planet: `PL_PLANET_TEXTURE_SLOT_COUNT = 5`.
- 4,096 bindless texture descriptors for the extension context.
- 100 pending residency request nodes per planet.
- 256 MiB default global staging buffer.
- 256 MiB default vertex cache **and** 256 MiB default index cache per planet.
- 256 MiB maximum decoded overlay crop per terrain tile.
- Resources longer than 1,024 pixels on their longest edge are expected to be
  downsampled by the resource path.
- dcapp reserves handle index zero and stores handle indices in `uint8_t`, so a
  context is effectively limited to 255 entries of each relevant handle type.

## 4. Build products and dependencies

The normal entry point is:

```sh
make build
# or
./scripts/build.sh -c release
```

The build produces separate dynamic modules for the two halves:

- `libpl_planet_processor_ext.{dylib,so}`
- `libpl_planet_ext.{dylib,so}`
- `libdcapp-planet-chunkgen.{dylib,so}`
- `libdcapp-planet-snapshot.{dylib,so}`

The generated platform build scripts compile the processor and runtime as
independent shared libraries. `dcapp-planet-chunkgen` includes the GDAL headers
and links `-lgdal`; the runtime extension does not. On the currently generated
scripts, macOS expects Homebrew GDAL under `/opt/homebrew/opt/gdal/include` and
uses Homebrew library paths. Linux expects `/usr/include/gdal` and the standard
x86-64 system library directory. The general setup documentation installs GDAL
with `brew install gdal` or `apt install libgdal-dev`.

Both applications are loaded by `pilot_light` rather than being conventional
standalone `main()` programs. The wrappers change into `pilotlight/out` and
launch an application by name. For chunk generation:

```sh
./bin/dcapp-planet-chunkgen.sh input.tif output-directory [options]
```

The wrapper makes the input absolute, creates the output directory, converts
both to paths relative to `pilotlight/out`, and invokes
`pilot_light -a dcapp-planet-chunkgen`. Snapshot uses an argument array instead
of reconstructing a command string and preserves absolute input/output paths.

At dcapp startup, [`apps/dcapp.c`](apps/dcapp.c) loads
`pl_planet_processor_ext` before `pl_planet_ext`. The processor remains a
runtime dependency because the renderer uses its `.chu` loader and shared
structures; it is not only an offline dependency.

## 5. End-to-end data flow

### Asset creation

1. Chunkgen opens the first raster band with GDAL.
2. It validates a nonrotated, square-pixel, projected polar stereographic CRS.
3. It reads sphere/projection parameters, affine geotransform, band scale,
   NoData, dimensions, and optionally exact statistics.
4. It partitions the raster into `tile_size` squares.
5. Each source value is quantized into a 16-bit grayscale PNG. Partial edge
   tiles are padded with the minimum elevation.
6. It writes `.planet.json`, with one projected center and `.chu` filename per
   tile.
7. The processor loads each PNG plus the neighbors needed for its east/south
   boundary and normal halo.
8. It dequantizes height, inverse-projects each sample to latitude/longitude,
   places it on a sphere, applies radial elevation, and computes a surface
   normal.
9. It preprocesses activation/error levels, builds every quadtree chunk's BTT
   mesh, and writes the `.chu` file in preorder.
10. Temporary PNGs are removed unless `--keep-tiles` is set.

### Runtime

1. `src/app/planet.c` parses `.planet.json`, resolves `.chu` paths relative to
   the metadata file, and constructs `plPlanetProcessInfo`.
2. `plPlanetI.create_planet` allocates the shared vertex/index GPU caches,
   scans every `.chu` into an in-memory quadtree, and queues every tile root.
3. dcapp records an initial `prepare()` call and submits it. Because prepare
   services only one request, this does not make every root resident.
4. Each frame, before views render, dcapp calls `prepare()` once per planet.
5. `prepare()` reads at most one requested chunk into the shared staging
   buffer, allocates cache ranges, and records buffer copies.
6. Rendering traverses visible chunks. It draws a resident parent while it
   requests missing children, and descends only when all four children are
   resident. Requests made during rendering are normally serviced next frame.
7. Each view renders to its own RGBA8 offscreen target with a D32S8 depth
   target. It then participates in dc_draw as an ordinary textured image.
8. Planet-specific 3D annotations render inside the same offscreen render pass;
   text and images that use 2D placement are queued for composition in the
   display draw flow.

This yields gradual refinement without geometric holes: a coarse parent is the
fallback until an entire four-child set is ready.

## 6. Offline stage: GDAL and `dcapp-planet-chunkgen`

### 6.1 CLI and defaults

The frontend accepts:

```text
dcapp-planet-chunkgen <input_dem> <output_dir>
  [--radius meters]
  [--tile-size pixels]                 default 4096
  [--min-height meters]
  [--max-height meters]
  [--meters-per-pixel meters]          default from geotransform
  [--tree-depth levels]                default 6
  [--max-base-error meters]            default 0.15 * meters_per_pixel
  [--prefix name]                      default input filename stem
  [--keep-tiles]
```

Chunkgen always sets `PL_PLANET_PROCESSING_FLAGS_DOUBLE_PRECISION` in generated
metadata and processor input. In this project, “double precision” means files
store every Cartesian coordinate as a high float plus a residual low float; it
does not mean the GPU executes 64-bit vertex arithmetic.

### 6.2 GDAL registration and side effects

The application calls `GDALAllRegister()` and sets
`GDAL_PAM_ENABLED=NO`. Disabling PAM prevents GDAL from leaving `.aux.xml`
sidecars next to input or temporary files. All raster operations use GDAL's C
API directly; no `gdalinfo`, `gdal_translate`, or subprocess is invoked.

Only band 1 is used. Extra bands are ignored.

### 6.3 Spatial-reference extraction

`GDALGetSpatialRef()` must return a spatial reference. The code accepts only a
projection name equal to GDAL's polar stereographic or stereographic constants.
Geographic/unprojected rasters, other map projections, rotated rasters, and
rasters without CRS metadata are rejected.

Projected linear units are converted to meters using `OSRGetLinearUnits`. If
the CRS unit factor is \(s_u\), every geotransform coefficient and false origin
is multiplied by \(s_u\).

The sphere radius defaults to `OSRGetSemiMajor()`. This intentionally treats
the CRS's semi-major axis as a spherical radius even if the source CRS is based
on an ellipsoid. The processor only implements `PL_DATUM_SPHERE` today.

Projection parameter extraction is:

- Latitude of origin, falling back to standard parallel 1. Its sign is retained
  but its magnitude is coerced to exactly 90 degrees. A magnitude below 45
  degrees is rejected.
- Central meridian, falling back to longitude of origin.
- Scale factor if present.
- False easting and northing, converted to meters.

If scale factor is absent, the code derives a spherical polar stereographic
factor from standard parallel \(\varphi_{ts}\):

\[
k_0 =
\begin{cases}
1, & |\varphi_{ts}| = 90^\circ \\
\frac{1+\sin|\varphi_{ts}|}{2}, & \text{otherwise.}
\end{cases}
\]

This follows the spherical relationship used by the inverse projection later;
it is not a general ellipsoidal polar stereographic implementation.

### 6.4 GDAL affine geotransform

For pixel/line coordinate \((p_x,p_y)\), GDAL defines:

\[
X = GT_0 + p_xGT_1 + p_yGT_2
\]

\[
Y = GT_3 + p_xGT_4 + p_yGT_5.
\]

Chunkgen requires \(|GT_2| < 10^{-12}\) and \(|GT_4| < 10^{-12}\), so there
is no rotation or skew. It uses

\[
mpp_x=|GT_1|s_u, \qquad mpp_y=|GT_5|s_u
\]

and rejects nonpositive values or relative disagreement greater than
\(10^{-6}\):

\[
|mpp_x-mpp_y| > 10^{-6}mpp_x.
\]

The auto-detected meters per pixel is \(mpp_x\). A user override changes the
processor's assumed sample spacing but does **not** change tile centers derived
from the source geotransform. Therefore an override inconsistent with the
GeoTIFF can distort tile extents relative to their origins.

The center of tile `(col,row)` is evaluated at the affine pixel coordinate

\[
p_x = col\,S + \frac{S}{2}, \qquad
p_y = row\,S + \frac{S}{2},
\]

where \(S\) is `tile_size`. Edge-tile centers use the full padded tile size,
not the smaller valid source rectangle.

### 6.5 Elevation scale, statistics, and NoData

Physical elevation is currently modeled as

\[
h_{meters} = raw \cdot scale.
\]

`scale` comes from `GDALGetRasterScale()` and defaults to 1 when absent or zero.
Negative scales are rejected. GDAL's per-band **offset is not read or applied**,
so a DEM whose physical conversion is `raw * scale + offset` will be wrong.

If either elevation bound is omitted, chunkgen calls
`GDALComputeRasterStatistics(..., FALSE, ...)`, requesting exact rather than
approximate statistics, and multiplies detected raw bounds by the band scale.
When writing tiles, the meter bounds are divided back by scale so quantization
operates in raw-band units.

GDALRasterIO reads each source window as `GDT_Float64` without resizing. NoData
is obtained from the band. A sample is replaced with the raw minimum when it is
NaN or exactly equal to the NoData value. Exact equality is the current rule;
there is no tolerance or mask-band handling in this function.

### 6.6 16-bit quantization

For raw bounds \(h_{min}, h_{max}\), every valid sample \(h\) becomes

\[
t = \operatorname{clamp}\left(
\frac{h-h_{min}}{h_{max}-h_{min}},0,1\right)
\]

\[
q = \operatorname{round}(65535t).
\]

If the range is empty, `q=0`. The processor reconstructs

\[
\hat h = H_{min} + \frac{q}{65535}(H_{max}-H_{min}),
\]

where capital \(H\) values are metadata bounds in meters. The maximum uniform
quantization step is

\[
\Delta h = \frac{H_{max}-H_{min}}{65535},
\]

and round-to-nearest error is at most \(\Delta h/2\), before floating-point and
projection errors.

The output is a full `tile_size` by `tile_size`, one-band UInt16 PNG made by
creating a GDAL MEM dataset and `GDALCreateCopy`-ing it with the PNG driver.
Partial source tiles retain minimum-height fill in their unused east/south
area. The intermediate PNG carries no copied georeferencing; placement comes
only from `plPlanetProcessInfo` and `.planet.json`.

### 6.7 Output orchestration

Chunkgen writes metadata before processing, writes all temporary PNGs, removes
any existing `.chu` target, then calls `plPlanetProcessorI.process()` once for
the entire tile grid. Existing chunk files are therefore forcibly rebuilt by
the normal frontend even though the processor itself skips output paths that
already exist. Unless `--keep-tiles` is passed, every temporary PNG is deleted
after the processor returns.

The tool prints success after `process()` because that API returns `void`.
There is no aggregate processor success result and no final verification that
every `.chu` exists and is readable.

### Owner notes: GDAL boundary

- Add band offset support before accepting datasets that rely on it.
- Decide whether exact statistics' NoData/mask behavior is sufficient for all
  supported GDAL drivers; tile conversion itself only handles scalar NoData and
  NaN.
- A nondefault `--meters-per-pixel` can make spacing disagree with geotransform
  origins. Either reject the mismatch or define resampling semantics.
- Projection name matching is intentionally narrow. Supporting a new CRS means
  adding both GDAL extraction and matching forward/inverse math throughout the
  processor/runtime adapter.
- `tile_size`, `tree_depth`, and shift widths need tighter semantic bounds. The
  processor stores grid coordinates in `int16_t` and uses expressions such as
  `1 << (depth * 2)`; large user values are not safe merely because chunkgen's
  allocation overflow checks pass.

## 7. Offline stage: terrain processor internals

### 7.1 Processor entry and tile context

`plPlanetProcessorI.process()` walks the declared tile array. For each missing
output file it builds a `plPlanetHeightMap` containing:

- The effective \(N\times N\) height grid.
- Elevation and projection parameters.
- Tile center in projected meters. The implementation stores projected X in a
  vector's X member and projected Y in its Z member.
- Per-sample integer grid coordinate, double height, double error, signed 8-bit
  activation level, temporary vertex-buffer index, and frame stamp.
- Neighbor/halo samples used for seams and normals.

The processor is CPU-only. Its “vertex buffer” is an in-memory array serialized
into `.chu`; no graphics API is involved offline.

### 7.2 Making a `2^L + 1` grid and stitching tiles

As explained earlier, the default 4096 source samples become a 4097-vertex
processor edge. The original tile is copied into the northwest portion of the
effective grid. The processor then opens neighboring PNGs by tile-grid index.
It copies shared east and south boundary data from those neighbors so adjacent
tiles calculate exactly the same position along their shared edge.

It considers north, northeast, east, southeast, south, southwest, and west
neighbors. Northwest is not needed by the chosen boundary/corner ownership.
Outer planet-dataset edges retain zero-valued quantized samples, which decode to
the configured minimum height.

Additional one-sample halos are retained around the height grid for central
difference normals. These samples are not emitted as mesh vertices.

The resulting default geometry spans 4096 intervals at `meters_per_pixel`:

\[
W_{tile}=(N-1)mpp=4096mpp.
\]

For grid coordinate \((i,j)\), local raster-plane coordinates are computed as

\[
x_{img}=\left((i+\tfrac12)-\tfrac12N\right)mpp,
\]

\[
z_{img}=\left((j+\tfrac12)-\tfrac12N\right)mpp.
\]

With \(N=4097\), `i=0` is exactly \(-2048mpp\) and `i=4096` is
\(+2048mpp\). This is why the tile center based on a 4096-pixel GDAL extent and
the processor's 4097-vertex grid agree at tile boundaries.

### Owner notes: halo implementation

The current halo code deserves regression tests before it is refactored. In
particular, east/south halo element coordinates are assigned `iSize + 1` in
places, and some neighbor reads use offsets such as source row 2. These may be
compensating for the processor's boundary ownership, but the intent is not
documented in code. Treat them as audit targets, not as a pattern to reproduce
blindly. A seam test should compare positions and decoded normals along every
horizontal, vertical, and four-tile corner boundary.

### 7.3 Spherical polar stereographic inverse

First convert the local raster row direction into northing and apply the tile
center and false origin:

\[
x=x_{img}+X_{tile}-FE,
\]

\[
y=-z_{img}+Y_{tile}-FN.
\]

Let

\[
\rho=\sqrt{x^2+y^2}, \qquad
c=2\tan^{-1}\left(\frac{\rho}{2Rk_0}\right).
\]

For a north-polar projection:

\[
\varphi=\frac{\pi}{2}-c,
\qquad
\lambda=\lambda_0+\operatorname{atan2}(x,-y).
\]

For a south-polar projection:

\[
\varphi=-\frac{\pi}{2}+c,
\qquad
\lambda=\lambda_0+\operatorname{atan2}(x,y).
\]

Longitude is adjusted once into \([-\pi,\pi]\). The equations are spherical;
the `plPlanetGeodeticModel` union has ellipsoid-shaped storage in its header but
only `PL_DATUM_SPHERE` is implemented.

### 7.4 Sphere and terrain positions

The unit surface direction under this project's axis convention is

\[
\hat{u}(\varphi,\lambda)=
(\cos\varphi\sin\lambda,
 \sin\varphi,
 \cos\varphi\cos\lambda).
\]

The base sphere point is

\[
P_s=R\hat{u}.
\]

The curved terrain point is radial displacement by decoded height:

\[
P_t=P_s+\hat{u}h=(R+h)\hat{u}.
\]

Both curved and flat axis-aligned bounds are stored per chunk. “Flat” in this
code does **not** mean the original map plane: its bounds are for the radius-only
sphere \(P_s\). The runtime flatten option radially removes height and therefore
uses these sphere bounds.

### 7.5 Surface normals

For a grid element, the processor gets Cartesian neighbor terrain points and
forms central-difference tangents:

\[
T_x=P_R-P_L, \qquad T_z=P_U-P_D.
\]

The normal is

\[
n=\operatorname{normalize}(T_z\times T_x).
\]

At a tile edge, one of these points comes from the neighbor halo. Correct halo
data is what makes lighting continuous even when the two tiles are separate
files and separate runtime roots.

The normal is octahedrally encoded into two floats. First project onto the
octahedron:

\[
n' = \frac{n}{|n_x|+|n_y|+|n_z|}.
\]

If \(n'_z\le 0\), fold the lower hemisphere:

\[
(e_x,e_y)=
((1-|n'_y|)\operatorname{sgn}(n'_x),
 (1-|n'_x|)\operatorname{sgn}(n'_y));
\]

otherwise \((e_x,e_y)=(n'_x,n'_y)\). Finally map from \([-1,1]\) into
\([0,1]\):

\[
encoded=\tfrac12(e+1).
\]

The vertex shader performs the inverse fold and normalizes the result.

### 7.6 Geometric error preprocessing

The processor starts with two binary-triangle-tree roots covering opposite
halves of the square. For each triangle with apex \(A\), right base endpoint
\(R_b\), and left base endpoint \(L_b\), it considers the base midpoint \(M\).
Recursion ends when the base spans at most one grid cell in both axes.

The implementation's error is

\[
e(M)=\lVert P_M\rVert-
\frac{\lVert P_{L_b}\rVert+\lVert P_{R_b}\rVert}{2}.
\]

Because \(\lVert P\rVert\approx R+h\), this is effectively radial midpoint
height error. It is **not** the Euclidean distance from \(P_M\) to the full 3D
edge segment. Curvature largely cancels because only vector lengths are used.

If \(|e|\ge e_0\), where \(e_0\) is `dMaxBaseError`, activation level is

\[
a=\left\lfloor
\log_2\left(\frac{|e|}{e_0}\right)+\frac12
\right\rfloor.
\]

The sample retains the maximum activation level ever assigned. Recursion then
continues into the two child triangles.

After both diagonal root halves are analyzed, a quadtree propagation pass
pushes activation dependencies from child centers to edge vertices and from
edge vertices to the parent center. This enforces the neighboring split
dependencies required to avoid T-junction cracks. The code calls the same
propagation function twice for each target level. That duplicate call is
current behavior and appears redundant; prove equivalence with mesh hashes or
seam tests before removing it.

### 7.7 Per-chunk binary triangle mesh generation

The quadtree is written root-first. Child order is:

1. top-left,
2. top-right,
3. bottom-left,
4. bottom-right.

A chunk covering \(2^l\) grid intervals is seeded with its two diagonal root
triangles. Mesh construction uses an explicit triangle stack. A triangle is
split when its base midpoint is forced by a mate dependency or its activation
level is at least the chunk's `iLevel`, until a maximum triangle level of
`2 * iLogSize` is reached.

The mate/base-edge hash forces the opposite triangle sharing a base edge to
split as well. This is the BTT diamond rule that prevents one side of an edge
from introducing a midpoint the other side does not contain.

After splitting, unique vertices are gathered with a frame stamp rather than
clearing a full map. Local UVs are

\[
u=\frac{i-i_{start}}{i_{end}-i_{start}}, \qquad
v=\frac{j-j_{start}}{j_{end}-j_{start}}.
\]

Each leaf triangle emits its indices in the implementation's
`apex, left, right` data order (the local stack members are accessed as
`e0,e2,e1`). Indices are 32-bit even for small chunks.

Every chunk is a complete, independently drawable mesh. Runtime refinement
does not stitch a parent mesh to a child mesh; it replaces a parent only when
all four children are resident, then draws the four-child set.

### 7.8 Float and high/low vertex formats

The single-precision on-disk vertex is:

```text
float position[3]
float oct_normal[2]
float uv[2]
```

which is normally 28 bytes.

The generated data uses the double/high-low form:

```text
float position_high[3]
float position_low[3]
float oct_normal[2]
float uv[2]
```

which is normally 40 bytes. For a CPU double component \(d\):

\[
h=(float)d, \qquad l=(float)(d-(double)h).
\]

Then \(h+l\) preserves substantially more absolute precision than one float.
It is a two-float expansion, not a bit-exact double representation.

## 8. The `.planet.json` and `.chu` formats

### 8.1 Metadata JSON

Chunkgen writes fields equivalent to:

```json
{
  "radius": 1737400.0,
  "meters_per_pixel": 400.0,
  "tile_size": 4096,
  "cols": 2,
  "rows": 2,
  "min_height": -9000.0,
  "max_height": 11000.0,
  "tree_depth": 6,
  "max_base_error": 60.0,
  "double_precision": true,
  "projection": {
    "type": "polar_stereographic",
    "latitude_of_origin": -90.0,
    "longitude_of_origin": 0.0,
    "scale_factor": 1.0,
    "false_easting": 0.0,
    "false_northing": 0.0
  },
  "tiles": [
    {"originX": 0.0, "originY": 0.0, "file": "prefix_0_0.chu"}
  ]
}
```

The dcapp loader validates positive radius, meters per pixel, tile size, grid
dimensions, and matching tile count. It resolves each relative `.chu` path
against the metadata file's directory. Tile ordering is row-major:

\[
index=column+row\cdot cols.
\]

The loader supports an older metadata form without a `projection` object. That
legacy path converts old latitude/longitude tile fields using compatibility
rules. New assets should always carry explicit projection metadata; otherwise
longitude and overlay orientation pass through historical transforms that are
hard to reason about.

### 8.2 Current `.chu` header

The current file starts with native binary values:

```text
int version_major
int version_minor
int version_patch
int tree_depth
double max_base_error       // float in older files
uint32 chunk_count
int processing_flags        // present in newer format revisions
```

Every chunk follows recursively in preorder:

```text
int label
int level
float normalized_x
float normalized_y
double curved_min[3]
double curved_max[3]
double flat_min[3]
double flat_max[3]          // bounds use float vectors in older files
uint32 vertex_count
vertex vertices[vertex_count]
uint32 index_count
uint32 indices[index_count]
children[4], if level > 0
```

There is no offset table. The loader scans the entire file once, records the
byte position of each payload in `szFileLocation`, and reconstructs parent and
four-child pointers from depth and preorder. It also overwrites/normalizes
`fX/fY` from topology to exact dyadic positions rather than trusting accumulated
file values. Runtime streaming later seeks directly to the recorded location.

The label is written but runtime drawing does not use it. `uIndex` is assigned
sequentially after load and indexes per-chunk texture-UV tables.

### 8.3 Format portability and compatibility behavior

The file writes native `int`, `uint32_t`, floats, doubles, and vector structs
with `fwrite`. It has no declared endianness, packing contract, byte-order
marker, checksum, payload byte count, or per-chunk validation hash. It should be
treated as a build-architecture-local format even if common 64-bit targets
happen to agree.

The loader compares file major and minor versions independently against the
extension version instead of performing a lexicographic semantic-version
comparison. It does not comprehensively check every `fread`, file length,
declared count, or recursive consistency. The runtime adapter currently ignores
the boolean result of `load_chunk_file` in its helper path.

### Owner notes: format evolution

Before introducing a new revision, define an explicit little-endian header with
magic bytes, header length, flags, chunk-table offsets, payload sizes, and a
checksum. Keep an isolated legacy reader. Validate all multiplication/addition
before allocating or seeking. Do not change struct fields and assume the old
native writer remains compatible.

## 9. Runtime metadata loading and planet creation

### 9.1 dcapp adapter objects

`DcAppPlanetContext` owns stretchy buffers for planets, views, breadcrumbs, and
GeoJSON. Index zero is reserved as undefined. Each public handle is a separately
allocated stable object containing an index into the corresponding context
array, so stretchy-buffer reallocation does not invalidate logic-facing
pointers.

The low-level extensions initialize lazily on the first relevant planet/view.
The adapter supplies the starter device and leaves staging size at its default.

`DcAppPlanetCreateInfo.mesh_cache_size_mb` is interpreted as a **combined**
mesh cache budget and split evenly into vertex and index buffers. A zero value
passes zero through and lets the extension choose 256 MiB for each buffer, so
zero means a 512 MiB combined default. XML's `MeshCacheSize` follows the same
combined split when explicitly supplied.

### 9.2 Low-level `plPlanet` creation

`create_planet()`:

1. Copies scalar process metadata but deliberately clears the copied
   `atTiles` pointer; it then owns its own tile/chunk-file arrays.
2. Chooses a 28- or 40-byte vertex stride from the processing flag.
3. Creates one device-local transfer-destination vertex buffer and one
   device-local transfer-destination index buffer.
4. Allocates and binds graphics memory for both buffers.
5. Creates 256-byte-aligned freelists over the buffers.
6. Initializes a 100-node residency request pool and an empty LRU sentinel.
7. Loads every `.chu` topology and initializes five texture indices and five
   per-chunk UV records per tile to the dummy texture/identity transform.
8. Queues the root chunk of every tile.

The initial light direction is `(-1,-1,-1)` unless the adapter/XML changes it.
The renderer normalizes it in the fragment shader.

dcapp records one immediate `prepare()` call in a temporary command buffer after
creation and submits it. Since only one request is serviced per call, a
multi-tile planet still needs later frames to make all roots resident.

### 9.3 Views

A view defaults to 1024 by 1024 if dimensions are omitted at the adapter level.
It owns:

- An RGBA8 UNORM color texture usable as render target and sampled texture.
- A D32_FLOAT_S8_UINT depth/stencil texture.
- A bind group used by dc_draw to composite the output as a 2D image.
- A dcDraw 3D list for planet annotations.
- Single/high-low and filled/wireframe pipeline variants.
- Runtime options: flags, `tau=0.3`, and overlay strength `0.3`.

The display XML runtime currently creates its model-owned planet views at a
fixed 1024 by 1024, even when the on-screen node is another size. Node size is
used for logical aspect/camera/display integration, while the offscreen raster
resolution stays fixed. Logic-created views honor the requested dimensions.

Views share the planet's residency queue, caches, LRU list, textures, and light.
They do not share camera, output target, shader selection, tau, flatten flag, or
annotation draw list.

## 10. Runtime streaming, request scheduling, and eviction

### 10.1 Request queue behavior

Each planet has exactly 100 preallocated request records. Requesting a chunk:

- If it is already resident, does nothing beyond later LRU touch on draw.
- If it is already queued, removes and reinserts its request at the queue head.
- If it is new and a free record exists, consumes that record and inserts at
  the head.
- If the queue is full, steals the tail record, reassigns it to the new chunk,
  and inserts it at the head.

`prepare()` services the head, so this is a most-recently-requested-first queue,
not FIFO. Traversal order matters: later requests can repeatedly move ahead of
older ones, and a full queue drops the least-recently-requested tail. The stored
request frame is not currently used for prioritization.

### 10.2 One upload per prepare call

dcapp calls `prepare()` once per planet near the beginning of each frame,
before any planet view is rendered. Each call processes **at most one chunk**:

1. Open the chunk's `.chu` file and seek to `szFileLocation`.
2. Skip metadata/bounds using the file version's field sizes.
3. Read vertex and index counts/payloads into CPU allocations.
4. Allocate vertex and index holes from the planet freelists.
5. If either allocation fails, evict eligible chunks and retry once.
6. Copy indices to staging offset 0 and vertices immediately after them.
7. Record barriers and staging-to-device-local buffer copies.
8. Free CPU payloads, remove the request, and put the chunk at the LRU head.

Requests discovered while rendering normally wait until the next frame. A
planet with \(n\) missing chunks therefore needs at least \(n\) prepare calls,
even if I/O and GPU bandwidth could handle more. Multiple views can generate
requests in the same frame, but do not increase service rate.

The global staging buffer is host-visible and host-coherent, so CPU writes do
not require an explicit flush. It is shared by all planets in the extension
context, but each prepare records copies into the command buffer in dcapp's
ordered frame flow.

### 10.3 Residency representation

A chunk is considered index-resident when `ptIndexHole != NULL`; uploaded chunks
normally also have `ptVertexHole`. The holes store byte offsets into the shared
per-planet cache buffers. Draw offsets are:

\[
vertexStart=\frac{vertexHole.offset}{vertexStride},
\qquad
indexStart=\frac{indexHole.offset}{4}.
\]

Because allocations use 256-byte alignment, internal fragmentation can be
significant for small chunks. Cache capacity is not simply sum of payload byte
counts.

### 10.4 LRU and safe eviction

Uploaded and drawn chunks are touched onto the LRU head. The tail is least
recently used. Eviction only considers chunks that:

- are not a tile root,
- are resident, and
- have no resident child.

That last constraint means eviction proceeds bottom-up. A parent cannot
disappear while a child depends on the quadtree transition. Making a chunk
unresident recursively unloads children, returns its freelist holes, removes it
from the LRU and any pending request, and clears residency pointers.

The current eviction helper performs multiple scans/passes. Its first pass
already walks eligible chunks without an age threshold until it gathers enough
vertex **and** index bytes, so later age-related passes offer less distinction
than their surrounding structure suggests. Treat the exact policy as current
mechanics, not a carefully tuned working-set algorithm.

Tile roots are never evicted. The combined size of all root payloads must fit
the configured cache buffers or the planet can remain permanently unable to
reach a valid base state.

### 10.5 Stream statistics

`get_stream_stats()` reports pending requests, resident chunks, total chunks,
and fallback chunks. `render_view()` resets the planet's fallback counter at the
start of each view. For a shared planet rendered through multiple views, the
reported fallback count therefore reflects the most recently traversed view,
not a sum across all views in the frame.

## 11. Visibility and CDLOD selection math

### 11.1 Bounds and closest distance

Traversal begins independently at every tile root. It chooses the curved or
radius-only AABB based on the flatten flag, casts its double components to
float, and applies an OBB-versus-perspective-frustum SAT test. A culled chunk is
not requested.

For a visible AABB, the renderer finds the closest point \(Q\) in the box to
the float camera position \(C\), then uses

\[
d=\lVert Q-C\rVert.
\]

This is distance to the chunk bounds, not center distance. The implementation
does not explicitly clamp zero distance before division in the LOD equation.

### 11.2 Pixel focal scale

Given vertical field of view \(\theta_v\) and aspect ratio \(a\), horizontal
field of view is

\[
\theta_h=2\tan^{-1}\left(\tan(\theta_v/2)a\right).
\]

For offscreen width \(W\), the horizontal pixel focal length is

\[
K=\frac{W}{2\tan(\theta_h/2)}.
\]

This is equivalent to the usual perspective focal scale expressed in pixels.

### 11.3 Current screen-space error

The implementation assigns chunk geometric error as

\[
E=e_{base}\cdot level,
\]

where `e_base` is the file-level maximum base error and `level` is `uLevel`.
The projected error metric is

\[
\rho=\frac{EK}{d}.
\]

This deserves emphasis: the runtime does **not** read a per-chunk maximum error,
and it does not use \(2^{level}\). Leaves at level 0 get \(E=0\); coarse roots
get the largest linearly scaled value. This is current project behavior rather
than canonical CDLOD error propagation.

### 11.4 Refinement and fallback state machine

Let subdivision threshold be \(\tau\), default 0.3, and merge threshold be

\[
\tau_m=0.5\tau.
\]

All four children must be resident before traversal may replace a parent. The
logic is:

```text
current chunk not resident
    request it, draw nothing

current chunk resident, children missing OR rho <= tau
    draw current chunk
    if children exist, missing, and rho > tau: count fallback
    if rho > tau: request all children
    if rho < tau/2: recursively unload children
    otherwise: retain current child residency

all four children resident AND rho > tau
    do not draw parent; recurse into all four children
```

The half-threshold provides a retention band, but there is no explicit
per-chunk “previously split” state. Residency itself acts as part of the
hysteresis state. Parents remain available while children draw, permitting a
quick fallback if children are later unloaded.

Because all four children replace one parent atomically, no empty quadrant
appears during streaming. The cost is overdraw-free but sometimes visibly
coarse fallback until the fourth child arrives.

### 11.5 SAT frustum culling in detail

The culling function transforms four adjacent AABB corners through the camera's
float view matrix. Their edge differences define an oriented box with center
\(C_b\), orthonormal axes \(A_i\), and half-extents \(e_i\).

For any candidate separating axis \(M\), the OBB interval is

\[
m_c=M\cdot C_b,
\]

\[
r_b=\sum_{i=0}^{2}|M\cdot A_i|e_i,
\]

\[
I_b=[m_c-r_b,m_c+r_b].
\]

At near distance \(z_n\), with \(t=\tan(\theta_v/2)\), near half-height and
half-width are

\[
y_n=z_nt, \qquad x_n=az_nt.
\]

For an axis \(M=(m_x,m_y,m_z)\), the near-plane projected half-width term is

\[
p=x_n|m_x|+y_n|m_y|.
\]

The frustum's near interval endpoints start as

\[
\tau_0=z_nm_z-p, \qquad \tau_1=z_nm_z+p.
\]

Depending on sign, the code scales the endpoint that extends toward the far
plane by \(z_f/z_n\). If the OBB and frustum intervals do not overlap on any
candidate axis, the chunk is culled.

The tested axes include:

- camera forward/depth,
- four side-plane normals,
- the three OBB axes,
- camera right crossed with each OBB axis,
- camera up crossed with each OBB axis,
- each of four frustum-edge directions crossed with each OBB axis.

Near-zero cross products are skipped with epsilon \(10^{-4}\). This is a fuller
SAT than six plane tests and handles cases where neither volume contains a
corner of the other.

The culler uses float-transformed world AABBs, while the vertex shader uses
high/low camera-relative math. Far from the origin, culling can therefore lose
precision before rendering does. A future large-world culling rewrite should
operate on double bounds relative to the double camera.

## 12. Runtime overlay texture slicing

### 12.1 Inputs and replacement semantics

Each planet has five slots shared by all its views. A `plPlanetTexture` provides
an image path, projected center \((X_c,Y_c)\), and meters per pixel \(m_t\).
The image dimensions are obtained after resource decoding.

Calling `set_texture()` first clears the old selected slot across every terrain
tile. Passing `NULL` is therefore the supported clear operation. A replacement
that later fails validation leaves the old texture cleared; replacement is not
transactional.

Required inputs are a valid slot, nonempty path, finite positive `mpp`, finite
origin, and a valid planet tile grid.

### 12.2 Projected rectangles and intersecting tile range

For image width \(W_i\) and height \(H_i\), the projected coverage is

\[
[X_c-\tfrac12W_im_t,\ X_c+\tfrac12W_im_t]
\]

by

\[
[Y_c-\tfrac12H_im_t,\ Y_c+\tfrac12H_im_t].
\]

Terrain tile width is currently

\[
W_T=uSize\cdot mpp_{terrain}.
\]

The runtime derives the planet grid's top-left projected boundary from tile 0's
center and half a tile. It uses floor/ceil arithmetic to choose the inclusive
row/column range whose rectangles can intersect the image, then clamps the
range to the declared grid. An image completely outside the terrain is a valid,
successful slot assignment with no per-tile resources.

### 12.3 Crop coordinate math

For each intersecting tile, rectangle intersection is converted back into
source pixels. X grows east/right:

\[
x_{src}=\frac{X-X_{image,min}}{m_t}.
\]

Image Y grows down while projected northing grows up:

\[
y_{src}=\frac{Y_{image,max}-Y}{m_t}.
\]

Floor/ceil create a half-open integer crop. When possible, one texel of gutter
is added around it to reduce filtering artifacts at chunk/tile boundaries.

The decoded crop must fit a 256 MiB per-terrain-tile byte limit:

\[
cropWidth\cdot cropHeight\cdot bytesPerPixel \le 256\text{ MiB}.
\]

Before resource loading, the implementation pads extremely thin crops so a
longest-edge downsample to at most 1024 does not round the other dimension to
zero. The target minimum short dimension is approximately

\[
\left\lceil\frac{longDimension}{1024}\right\rceil.
\]

It writes every crop as a generated PNG named like
`hazard_prep_<planet>_<slot>_<tileX>_<tileY>.png`, loads it through the resource
manager without caching, requests residency, and assigns a bindless index.
There is no matching `remove()` for these generated files in the current
extension, so they persist on disk until something outside this subsystem
cleans or overwrites them.

### 12.4 Chunk-local UV transform

Every chunk stores normalized position `(fX,fY)` within its tile and local mesh
UV \((u,v)\in[0,1]^2\). Let

\[
d=treeDepth-uLevel-1
\]

be top-down depth and

\[
f=2^{-d}
\]

be the chunk's fraction of tile width. Let \(S_p\) be the full terrain tile
width measured in source-overlay pixels, \((o_x,o_y)\) its source-image top-left,
\((c_x,c_y)\) the final crop origin, and \((W_c,H_c)\) crop dimensions. The GPU
transform is:

\[
scale_x=\frac{S_pf}{W_c}, \qquad
scale_y=\frac{S_pf}{H_c},
\]

\[
offset_x=\frac{o_x+f_XS_p-c_x}{W_c}, \qquad
offset_y=\frac{o_y+f_YS_p-c_y}{H_c}.
\]

The fragment shader samples

\[
uv_{crop}=uv_{chunk}\odot scale+offset.
\]

Normalized coordinates remain valid if the resource manager uniformly
downsamples the crop. The runtime initializes missing slots to identity UV and
a transparent 2-by-2 dummy texture.

### 12.5 Bindless resource lifecycle

The extension keeps a texture-handle-to-bindless-index map. A new resident GPU
texture consumes an index and updates binding 1 of every frame's set-0 bind
group. Releasing a tile crop evicts/unloads the resource and returns the index
through the map allocator. The old descriptor is not immediately overwritten;
all chunk records are switched to the dummy index, and a later reuse updates
that descriptor slot.

The pool/layout permits 4,096 sampled textures. Exhaustion is guarded by
assert/TODO-style behavior rather than a graceful user-facing error path.

### Owner notes: overlays

- Per-tile crops multiply resource count. One logical five-slot image over many
  terrain tiles can consume many bindless indices, not just one.
- Individual crop failures can be skipped while the overall setter succeeds,
  leaving a partially populated logical texture.
- Generated PNGs are not deleted by the current extension. Replace the
  filesystem handoff or own a cleanup manifest across success/failure paths.
- Replacement should ideally stage new slices and commit only after all required
  resources succeed.
- Texture placement assumes tile 0 plus a regular row-major grid. Arbitrarily
  spaced tile origins are not honored by range selection.

## 13. GPU architecture

### 13.1 Extension-global GPU state

`pl_planet_ext` has one global context for its initialized device. It owns or
coordinates:

- A render-pass layout with depth/stencil attachment 0 and RGBA8 color
  attachment 1.
- A bind-group pool and set-0 bindless texture layout.
- One set-0 bind group per in-flight frame.
- One linear clamp-to-border sampler.
- A transparent 2-by-2 RGBA32F dummy texture.
- A handle-to-bindless-index map with a 4,096-entry ceiling.
- A host-visible, host-coherent staging buffer, default 256 MiB.
- Dedicated and buddy GPU memory allocators used for view textures.
- Scratch CPU storage and profiling/stat counters.
- A dynamic-data block reference supplied by the graphics backend each frame.

The planet extension is therefore device-singleton state. It is not safe to
initialize independent devices into the same extension instance.

### 13.2 Render-pass contract

The offscreen render pass has:

| Attachment | Format | Role |
| --- | --- | --- |
| 0 | `D32_FLOAT_S8_UINT` | Depth/stencil target |
| 1 | `RGBA8_UNORM` | Sampled planet-view output |

The color attachment transitions back to a sampled layout after the pass so
dc_draw can composite it. The view creation command buffer also performs the
initial output/depth layout transitions.

Depth is reverse-Z:

- depth clear value is 0,
- filled pipelines use `GREATER_OR_EQUAL`,
- filled terrain writes depth,
- 3D annotation drawing is configured for reverse-Z depth testing/writes,
- the camera projection helpers are the reverse-Z variants used by the rest of
  the rendering stack.

Reverse-Z places the near plane toward depth 1 and the far plane toward 0,
improving floating-point depth distribution over planet-scale ranges. Changing
the compare function, clear value, or projection convention in isolation will
make geometry disappear or invert occlusion.

### 13.3 Bind-group layout

Set 0 is shared across planet draws:

```text
binding 0: sampler, fragment stage
binding 1: texture2D[4096], fragment stage, nonuniform/bindless indexing
```

Set 3 binding 0 is one dynamic uniform record per chunk draw. Sets 1 and 2 are
unused by the default planet shaders but remain part of the broader graphics
convention.

Every in-flight frame gets a separate set-0 bind group, but all groups reference
the same sampler and logical bindless array. When a new texture index is
assigned, the extension updates every frame group's descriptor at that index so
future frame rotation sees a consistent table.

### 13.4 Per-planet GPU state

Each `plPlanet` owns one large vertex buffer and one large index buffer. Both are
device-local and transfer destinations. Chunks do not create buffers; a
resident chunk occupies freelist subranges of these pools.

This design makes draw submission cheap and avoids thousands of GPU objects,
but it creates three constraints:

1. Buffer offsets must remain aligned and convertible to vertex/index starts.
2. Fragmentation can cause allocation failure even when total free bytes look
   sufficient.
3. A view cannot outlive the shared cache it draws from.

Texture crops are resource-manager textures and bindless descriptors rather
than suballocations in the mesh caches.

### 13.5 Per-view GPU state and four pipelines

Each view creates four graphics pipeline/shader variants:

| Position encoding | Rasterization | Selected when |
| --- | --- | --- |
| one float3 | filled/back-face cull | non-double file, no wireframe |
| high float3 + low float3 | filled/back-face cull | double flag, no wireframe |
| one float3 | polygon wireframe | non-double file, wireframe |
| high float3 + low float3 | polygon wireframe | double flag, wireframe |

The high/low shader option macro is spelled
`PL_PLANET_DOUBLE_PRECISON` (missing the second `I`) in both C pipeline creation
and GLSL. The matching typo is part of today's compile contract.

Filled pipelines write reverse-Z depth and cull back faces. Wireframe uses
polygon wire mode, disables depth writing, and uses an always depth comparison
in the current pipeline setup; it is a diagnostic visualization rather than a
physically occluded equivalent of the filled pass.

Custom vertex/fragment shader paths are retained by the adapter, mounted into
the VFS, and compiled through the shader extension. A shader change reloads the
view variants; XML can declare indexed fragment variants and runtime variables
select among them.

### 13.6 Per-draw dynamic data

Each drawn chunk allocates a dynamic binding and writes `plGpuDynPlanetData`:

```text
int   level
int   flags
uint  textureIndex0
int   chunkID
vec4  uvScaleOffset0
vec3  lightDirection
float radius
float overlayStrength
int   padding[3]
vec4  cameraHigh
vec4  cameraLow
mat4  cameraViewProjection
uint  textureIndex1..4
vec4  uvScaleOffset1..4
```

The shader-compatible payload is 240 bytes under the interop layout and is
statically asserted to fit within 256 bytes. Dynamic uniform alignment commonly
makes each chunk draw consume a full 256-byte slot.

The view projection stored here is

\[
M_{VP}=M_P M_{V,double}.
\]

The vertex passed to it is already camera-relative. `tViewMatDouble` is the
camera view rotation/orientation representation used for this relative-space
path; the ordinary world translation must not be applied a second time.

The chunk ID used for debug coloring is currently

```text
chunk.uIndex + chunk.uFileID
```

rather than a collision-free global prefix sum, so different tile/chunk pairs
can share debug colors/IDs.

### 13.7 Command recording order and synchronization

Streaming CPU code copies payloads into coherent staging memory, records buffer
barriers, then records two transfers into the device-local index/vertex pools.
The render command buffer later reads those buffers as index/vertex input. The
graphics abstraction owns the backend-specific barrier translation.

During a dcapp frame, planet preparation occurs before node rendering. Node
rendering queues the planet view image into dc_draw, then the planet scope flush
records the offscreen planet render before the main render pass submits the
queued composite. Although the 2D image was logically queued first, GPU command
submission order makes the offscreen output available before it is sampled in
the main pass.

The view's 3D dcDraw list is submitted inside the same offscreen render pass
after terrain, so annotations share its depth target. At render end, the color
target is transitioned for sampling.

### Owner notes: GPU lifetime

- Review explicit destruction of the dummy texture, sampler, bind-group layout,
  and per-view shader objects. Some may be owned/deferred by Pilot Light
  managers, but the extension cleanup paths do not make every ownership
  transfer self-evident.
- Shader reload queues old shaders for deletion when handles are present, while
  view cleanup is less explicit. Verify resource-manager conventions before
  changing hot reload.
- The staging upload path does not currently prove
  `indexBytes + vertexBytes <= stagingBufferSize` before writing. This is a
  critical bounds check to add.
- The broad render-pass dependency masks are conservative. Tightening them
  requires backend validation across Vulkan/Metal equivalents.

## 14. Vertex shader, large-world precision, and flattening

### 14.1 Vertex inputs

Single-position layout:

```text
location 0: vec3 position
location 1: vec2 oct normal
location 2: vec2 local UV
```

High/low layout:

```text
location 0: vec3 positionHigh
location 1: vec3 positionLow
location 2: vec2 oct normal
location 3: vec2 local UV
```

The high/low path reconstructs conceptually

\[
P=P_h+P_l.
\]

Doing that as a naive float addition too early would lose the residual. The
shader keeps the pieces for compensated camera subtraction.

### 14.2 Splitting the camera

For each double camera component \(c\), CPU code computes

\[
c_h=(float)c, \qquad c_l=(float)(c-(double)c_h).
\]

The goal is an accurate relative displacement

\[
D=(P_h+P_l)-(C_h+C_l)
\]

even when \(P\) and \(C\) are millions of meters from the origin but only a few
meters apart.

### 14.3 Compensated subtraction used by GLSL

The exact shader sequence is:

\[
t_1=P_l-C_l
\]

\[
e=t_1-P_l
\]

\[
t_2=((-C_l-e)+(P_l-(t_1-e)))+P_h-C_h
\]

\[
D_h=t_1+t_2
\]

\[
D_l=t_2-(D_h-t_1)
\]

\[
D=D_h+D_l.
\]

This is an error-compensated sum/difference arrangement. The parenthesized
terms recover rounding residuals from subtracting the low components; the
high-component difference is added while values are camera-local. The final
relative vector has much better precision than `float(P) - float(C)`.

Clip position is

\[
P_{clip}=M_{VP}(D_x,D_y,D_z,1)^T.
\]

### 14.4 Flattening

When `PL_PLANET_FLAGS_FLATTEN` is set, the shader replaces the terrain render
position with radial projection onto the base sphere:

\[
P_{render}=R\frac{P_t}{\lVert P_t\rVert}.
\]

The shader then assigns `worldHigh = renderWorldPos` and
`worldLow = renderWorldPos - worldHigh`. All three are `vec3`, so the latter is
normally exactly zero: flattening discards the original two-float expansion
after doing the nonlinear normalize/multiply in GPU float math.

Two subtle outputs remain unflattened:

- `tWorldPosition` sent to the fragment shader is the original terrain
  `P_h+P_l` float sum.
- The decoded normal remains the terrain normal, not the radius-sphere normal.

The default fragment shader only uses that position for dead/diagnostic
latitude/longitude calculations and uses the terrain normal for lighting.
Consequently a flattened view can retain relief-like lighting even though its
geometry lies on the sphere. That may be useful visually, but it is not a fully
geometrically flattened material model.

### 14.5 Normal decode

The encoded pair is first remapped:

\[
f=2encoded-1.
\]

Construct

\[
n=(f_x,f_y,1-|f_x|-|f_y|).
\]

Let \(t=\max(-n_z,0)\). The lower fold adjusts X/Y toward zero with the sign
opposite each component, then normalizes. This is the inverse of the processor's
octahedral map.

### 14.6 Debug colors

The vertex shader contains a fixed palette of 16 colors. It emits a color based
on `level % 16`, or `chunkID % 16` in show-chunks mode. The fragment shader adds
this color for show-level/show-chunk modes. Wireframe replaces material output
with the debug color. Show-origin is not a shader mode: CPU render code adds a
dcDraw transform gizmo at the Cartesian origin, scaled to 1.2 planet radii.

## 15. Fragment shader, lighting, and texture composition

### 15.1 Lighting equation

The default shader decodes and normalizes \(N\), normalizes configured light
direction \(L\), and computes

\[
C_{terrain}=0.5\max(0,N\cdot L).
\]

Sun color is white and ambient is exactly zero. There is no albedo terrain
texture, gamma conversion, shadow, specular term, atmosphere, exposure, or tone
mapping inside this shader. Output alpha starts at 1.

The setter is named light direction, and the shader uses it directly as the
incoming-light dot direction. If callers conceptually provide a ray direction
from the light toward the surface, they need the opposite sign.

### 15.2 Five overlay slots

For each slot \(s\), the shader computes

\[
uv_s=uv\odot scale_s+offset_s
\]

and samples a bindless texture. Samples are simply added:

\[
H=\sum_{s=0}^{4} texture_s(uv_s).
\]

Then:

\[
H'=strength\cdot H
\]

and final RGB is

\[
C=C_{terrain}+0.3H'_{rgb}.
\]

At the default `strength=0.3`, the effective multiplier on summed overlay RGB
is \(0.3\times0.3=0.09\). Overlay alpha is ignored. This is additive emission-
like coloring, not alpha compositing:

\[
C \ne (1-\alpha)C_{terrain}+\alpha C_{overlay}.
\]

Multiple opaque slots can push values above 1 before later framebuffer/display
handling.

### 15.3 Dummy textures and borders

Every absent slot points at a transparent RGBA dummy. Sampling uses linear
min/mag filtering, mip range 0 to 1, and clamp-to-border with transparent-black
border in U and V. Together with gutters, this prevents an overlay from
smearing across an unrelated crop edge.

### 15.4 Dead/default-shader code

The fragment shader still computes spherical latitude/longitude and declares a
hard-coded lunar radius, degree conversion constants, and variables used only
by commented experiments. Large commented blocks contain an older in-shader
polar texture projection. Current placement is entirely CPU-generated UV
scale/offset; those calculations do not affect output. Removing them should be
safe only after custom shader include/derivation expectations are checked.

## 16. How dcapp owns and drives the extension

### 16.1 Startup

`apps/dcapp.c` loads, in relevant order:

1. Pilot Light/platform extensions.
2. `dc_draw_ext` and `dc_draw_backend_ext`.
3. `pl_planet_processor_ext`.
4. `pl_planet_ext`.

It preprocesses XML, creates model-side contexts including
`DcAppPlanetContext`, then bootstraps the window and renderer. Runtime VFS mounts
include terrain shaders, assets, shader temp/output locations, and tile data.
The resource manager and shader system are initialized before runtime planet
textures/shaders need them.

The logic module receives dcapp-owned API tables through
`DcAppDisplayLogicInit`: application, draw, mouse, texture, and planet. It does
not receive the raw `plPlanetI` extension pointer.

### 16.2 Adapter API layers

There are three layers that are easy to confuse:

```text
logic module
  -> DcAppPlanetApi / DcAppDrawApi
      -> src/app/planet.c and src/app/draw.c
          -> plPlanetI
```

`DcAppPlanetApi` creates/finds planets and views, places/clears texture slots,
sets light/shaders, and owns GeoJSON/breadcrumbs. `DcAppDrawApi` creates a
per-frame drawn planet-view instance with a camera and queues annotations into
that instance. A persistent `DcAppPlanetViewHandle` is not itself a drawn view;
`planet_view_geodetic/cartesian()` returns the per-frame draw handle used by
annotation calls.

### 16.3 Declarative XML path

`src/app/xml.c` parses a top-level `<Planet>` definition and its children.
The definition itself is model/resource state, not a drawable scene node. Its
children can declare data files, up to five textures in declaration order,
indexed shaders, and views. View references are resolved while parsing, so the
referenced planet definition must already be known at that point.

At runtime, the model creates one shared adapter planet for each definition and
then its views. Although model storage can contain multiple PlanetData entries,
the current creation path uses only the first data file for a shared planet.

Per-frame runtime update:

- observes texture enabled/refresh variables and replaces or clears slots,
- observes light variables and updates shared light direction,
- calls `prepare()` once for each planet.

When a view node renders, runtime:

- resolves desired shader index and reloads defaults/custom paths on changes,
- updates tau and flatten flags,
- constructs a camera from the view variables,
- queues the offscreen output in dc_draw,
- traverses child annotation nodes into that drawn view.

At the end of the appropriate render scope, queued planet views are flushed into
their offscreen textures before the main display pass samples them.

### 16.4 Programmatic logic path

The sample at [`samples/planet/logic/logic.c`](samples/planet/logic/logic.c)
demonstrates the complete lifecycle:

1. Build an absolute `.planet.json` path.
2. `create_planet_with_id()` with a combined mesh cache budget.
3. Create a geodetic 1024-square view.
4. Load GeoJSON and create a breadcrumb store.
5. Update shared light and texture slots in `display_draw()`.
6. In a draw callback, call `planet_view_geodetic()` to queue the view and
   obtain a frame-local drawn-view handle.
7. Queue GeoJSON, ellipses, lines, spheres, local shapes, and text against that
   handle.
8. Change fragment shaders only when the desired selection changes.

Logic-owned objects are registered in the shared planet context and cleaned up
when the application tears down; the sample's `display_close()` does not
manually free them.

### 16.5 Frame order

The relevant application update sequence is:

```text
logic/value update
dc_draw backend new_frame
dc_app_draw_context_begin
dc_app_renderer_update_planets       # texture/light update + one prepare
dc_app_renderer_render               # queue views and annotations
dc_app_draw_context_end              # planet scopes have recorded outputs
flush deferred variable sets
begin main render pass
dc_app_draw_context_submit            # composite planet texture with other UI
end main pass / end frame
commit interaction state
```

LOD requests arise during `dc_app_renderer_render`, after this frame's prepare.
That one-frame scheduling boundary is intentional in current flow.

### 16.6 Shutdown order

After the device is flushed, dcapp destroys planet-owned auxiliary objects and
views first, then planets, then cleans up the low-level planet extension. The
planet context is destroyed before the resource manager and underlying rendering
extensions. This ordering allows texture eviction, shader cleanup, buffer
destruction, and bind-group cleanup to call still-live dependencies.

## 17. Camera and coordinate-frame math

### 17.1 Geodetic to Cartesian and inverse

For latitude \(\varphi\), longitude \(\lambda\), elevation \(h\), and spherical
radius \(R\):

\[
r=R+h
\]

\[
x=r\cos\varphi\sin\lambda,
\quad y=r\sin\varphi,
\quad z=r\cos\varphi\cos\lambda.
\]

Inverse conversion is:

\[
r=\sqrt{x^2+y^2+z^2},
\quad \varphi=\arcsin(y/r),
\quad \lambda=\operatorname{atan2}(x,z),
\quad h=r-R.
\]

The API stores a geodetic point in a generic `Vec3d` as latitude, longitude,
elevation. Do not interpret it as Cartesian without checking the associated CRS.

### 17.2 Forward polar stereographic used for runtime placement

Runtime texture placement converts a geodetic center back to the same projected
meter system. With \(\Delta\lambda=\lambda-\lambda_0\):

North pole:

\[
\rho=2Rk_0\tan\left(\frac\pi4-\frac\varphi2\right)
\]

\[
x=FE+\rho\sin\Delta\lambda,
\quad y=FN-\rho\cos\Delta\lambda.
\]

South pole:

\[
\rho=2Rk_0\tan\left(\frac\pi4+\frac\varphi2\right)
\]

\[
x=FE+\rho\sin\Delta\lambda,
\quad y=FN+\rho\cos\Delta\lambda.
\]

These are algebraically paired with the processor inverse in section 7. A
round-trip test across poles, central meridians, false origins, and nonunit
scale factors is one of the most valuable regression suites for this subsystem.

### 17.3 Local north/east/down basis

At \((\varphi,\lambda)\), dcapp constructs:

\[
N=(-\sin\varphi\sin\lambda,
   \cos\varphi,
  -\sin\varphi\cos\lambda),
\]

\[
E=(\cos\lambda,0,-\sin\lambda),
\]

\[
U=(\cos\varphi\sin\lambda,
   \sin\varphi,
   \cos\varphi\cos\lambda),
\qquad D=-U.
\]

The basis is used for geodetic camera attitudes, local surface annotations, and
some label orientation/projection work.

### 17.4 Geodetic camera attitude

At zero attitude, forward points down and up points north. The implementation
applies local yaw about down, pitch about the yawed right/east direction, and
roll about the resulting forward direction using Rodrigues rotation:

\[
v'=v\cos\theta+(k\times v)\sin\theta+k(k\cdot v)(1-\cos\theta).
\]

This makes yaw a heading in the local tangent plane, rather than a global-Y
rotation. Signed roll helpers use

\[
\theta=\operatorname{atan2}
\left(k\cdot(a\times b),a\cdot b\right).
\]

Cartesian camera mode instead delegates position and roll/pitch/yaw to the
Pilot Light camera convention.

### 17.5 Perspective and orthographic setup

Default view parameters are 60-degree vertical FOV, near 1 meter, and far
\(10^8\) meters unless supplied otherwise by the path. Aspect comes from the
logical display dimensions passed to `render_view`, which can differ from the
offscreen texture's fixed XML resolution.

For orthographic geodetic views, altitude is approximated as

\[
h_c=\lVert C\rVert-R.
\]

The half extents are chosen to match a perspective footprint at that distance:

\[
H_{half}=h_c\tan(\theta_v/2),
\qquad W_{half}=aH_{half}.
\]

The custom reverse-Z orthographic matrix uses reciprocal half extents for X/Y
and a Z scale based on the near/far range. This approximation assumes the
camera's radial altitude is meaningful and does not intersect terrain.

### 17.6 Legacy metadata compatibility

Metadata without an explicit projection follows older longitude/orientation
conversions, including a `180 - longitude` style mapping in one adapter path and
a historical overlay Y flip. The old tile-origin conversion and overlay path do
not express every transform identically. Treat this as compatibility behavior,
not the coordinate contract for new data. When deleting it, migrate actual old
assets and snapshot parsing at the same time.

## 18. Planet annotations, local geometry, GeoJSON, and breadcrumbs

### 18.1 Two annotation classes

Geometry such as spheres, polylines, polygons, filled convex polygons, and
ellipses is converted to planet Cartesian coordinates and queued into the
view's 3D dcDraw list. It is submitted after terrain in the offscreen pass and
can depth-test against terrain.

Text and image placement involves a world-to-screen step and is ultimately
composed as screen-oriented content. Its occlusion/visibility helpers first
reject points hidden behind the sphere.

The low-level `plPlanetI.draw_text` entry instead adds 3D text directly to the
view draw list after a float ray/sphere test. The current dcapp-facing
`planet_text_*` APIs do not call that path; they use the projected 2D label path.
An internal `dc_app_draw_planet_text()` wrapper still exposes the low-level
route but has no call sites in the current tree.

### 18.2 Local surface container

`planet_container_push_geodetic()` lets logic author a small planar shape once
and place it on the sphere. Starting from an anchor unit up vector \(U\), north
and east tangent basis, local 2D point \(p\) is scaled and rotated in the tangent
plane. Let the resulting tangent displacement be \(T\), distance
\(d=\lVert T\rVert\), and angular arc

\[
\alpha=d/R.
\]

The displaced unit direction is the spherical exponential-map form

\[
U'=U\cos\alpha+\frac{T}{d}\sin\alpha,
\]

and the output point is

\[
P=(R+h)U'.
\]

This maps local meters to great-circle distance instead of adding a tangent
vector and leaving the result off the sphere. The approximation semantics for
very large shapes remain that of a radial sphere, not ellipsoidal geodesics.

### 18.3 Sphere occlusion and label projection

To determine whether a world point is hidden by the planet, the renderer tests
the camera-to-point segment/ray against a radius sphere using the quadratic

\[
\lVert O+tD\rVert^2=R^2,
\]

or

\[
(D\cdot D)t^2+2(O\cdot D)t+(O\cdot O-R^2)=0.
\]

A nearer positive intersection means the label point is behind the sphere.
Visible positions are transformed to clip/NDC and then pixel coordinates.

Perspective text size uses the approximate projected-size relation

\[
pixels \approx
\frac{worldSize\cdot viewportHeight}
     {2d\tan(\theta_v/2)}.
\]

The current path clamps it into a 1-to-500-pixel range, then accounts for the
logical displayed area.

### 18.4 GeoJSON

The adapter loads GeoJSON into CPU-side geometry and style-independent feature
data. At draw time, dcapp converts geodetic coordinates to the planet's
Cartesian system and emits line/fill primitives according to style flags,
height-above-terrain, line width/pattern, and colors. Style selection belongs to
dcapp; `pl_planet_ext` only receives ordinary draw primitives.

### 18.5 Breadcrumbs

Breadcrumb storage has a declared CRS, maximum point count, and minimum point
spacing. Geodetic updates are converted to Cartesian for spacing comparison.
The spacing test is Euclidean chord distance:

\[
d=\lVert P_{new}-P_{last}\rVert,
\]

not spherical arc distance. For nearby samples chord and arc are close; over a
large angular separation chord is shorter. When capacity is reached, the
implementation shifts out the oldest point and appends the new one. Retrieval
returns points in the breadcrumb's declared CRS for drawing/API use.

## 19. Snapshot renderer

[`apps/dcapp_planet_snapshot.c`](apps/dcapp_planet_snapshot.c) is a focused
consumer of the same processor loader and runtime extension. It is useful both
as a product tool and as an integration test because it bypasses XML, display
layout, and the main dcapp compositor.

Typical invocation is:

```sh
./bin/dcapp-planet-snapshot.sh \
  --planet-data output/prefix.planet.json \
  --crs geodetic \
  --attitude-frame local-ned \
  --lat -70 --lon 0 --elevation 500000 \
  --yaw 0 --pitch 0 --roll 0 \
  --width 1024 --height 1024 --fov 60 \
  --output snapshot.png
```

Cartesian position with `cartesian-rpy` is the other supported camera form.
Optional custom vertex and fragment shader paths use the same planet shader
contract.

Snapshot initializes only the needed starter, shader, dcDraw backend, resource,
processor, and planet systems. It creates a view at the requested output size,
uses near 1/far \(10^8\), and overrides tau to 0.05 for a more refined image.

Every frame it calls `prepare()`, renders, then checks:

```text
pending requests == 0 AND fallback chunks == 0
```

The condition must remain true for three consecutive frames. It then flushes
the device, copies the sampled RGBA texture into a host-visible coherent buffer,
flushes again, and writes four-channel PNG data with `stbi_write_png`.

“Settled” is view-relative: culled chunks can remain nonresident, which is
correct for a snapshot. It also depends on the last rendered view's fallback
counter, but snapshot has exactly one view.

Snapshot contains its own `.planet.json` parsing rather than calling the full
dcapp adapter loader. That duplication is a drift risk: any metadata schema or
legacy compatibility change must be applied to both paths or extracted into a
shared module.

## 20. Memory, performance, and latency model

### 20.1 Default persistent GPU memory

At default settings, approximate large allocations are:

| Owner | Allocation |
| --- | ---: |
| Extension | 256 MiB shared staging buffer |
| Each planet | 256 MiB vertex buffer |
| Each planet | 256 MiB index buffer |
| Each view | \(4WH\) bytes RGBA8 color, excluding allocator overhead |
| Each view | backend storage for D32S8 depth/stencil, commonly \(8WH\) bytes |
| Each resident overlay crop | resource-dependent RGBA pixels/mips |

Thus one default planet already reserves roughly 512 MiB of device-local mesh
cache, plus the extension's 256 MiB staging allocation. Two planets do not share
mesh cache even when they refer to the same asset.

An explicit dcapp cache value \(M\) MiB is split approximately into

\[
B_v=B_i=\frac{M\cdot 1024^2}{2}.
\]

Because the fields are `uint32_t`, very large MiB values require overflow
validation before conversion.

### 20.2 CPU metadata cost

All chunk topology is resident on CPU from planet creation until cleanup. Per
chunk this includes pointers to parent/four children, two double AABBs pairs,
file location, LRU links/state, GPU-hole pointers, and indices. In addition,
each tile allocates five `vec4` UV records per chunk:

\[
5\cdot16=80\text{ bytes/chunk}
\]

just for overlay transforms, before chunk-struct padding and arrays. At depth 6
this is 109,200 UV bytes per tile; topology dominates further. Raising tree
depth multiplies chunk count by approximately four per added level.

### 20.3 Disk and streaming I/O

`.chu` repeats a complete independent mesh for every quadtree node. Parent and
child vertices are not deduplicated across chunks. File size is approximately

\[
header+\sum_c
\left(metadata_c+n_{v,c}\cdot stride+n_{i,c}\cdot4\right).
\]

The runtime opens the file, seeks, allocates CPU arrays, and reads one chunk for
each serviced request. There is no persistent file handle, memory map, async I/O,
decompression worker, or batched upload. This keeps control flow simple but can
produce main-thread latency spikes on slow storage.

### 20.4 Refinement latency

Since the service rate is one chunk per planet per `prepare()` call, a parent
that needs four absent children cannot refine in fewer than four frames under
normal dcapp scheduling. Queue competition, allocation eviction, or multiple
visible regions can increase this. At 60 Hz, the theoretical minimum for four
children is about 67 ms from the first service opportunity, plus the frame in
which requests were discovered.

The request cap can also discard old requests. Traversal reissues requests for
visible missing chunks, so progress generally continues, but strict fairness is
not guaranteed.

### 20.5 CPU render cost

Every view traverses the quadtree independently. Shared residency prevents
duplicate data, but it does not share visibility or LOD results. Approximate CPU
work is proportional to visible/visited nodes across all views:

\[
O\left(\sum_{views}visitedChunks_{view}\right).
\]

Each drawn chunk allocates one dynamic uniform slot and emits one indexed draw.
There is no multi-draw, meshlet, indirect-draw, or GPU culling path.

### 20.6 Overlay setup cost

Setting a logical overlay decodes the full image, computes every intersecting
tile crop, copies pixels, writes temporary PNGs, synchronously asks the resource
manager to load/make resident, and updates bindless descriptors. Do not call it
every frame. Both XML runtime and the logic sample track refresh/enable state so
they update only on change.

## 21. Failure modes and current owner audit

This section is intentionally direct. It separates known current behavior from
features an owner might reasonably assume exist.

### 21.1 Asset-generation and GDAL risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Only first band is read. | Multiband DEM conventions are ignored. | Validate/choose a band explicitly in the CLI. |
| Band scale is applied but band offset is ignored. | Absolute elevation is wrong for offset-encoded DEMs. | Implement `raw * scale + offset` consistently in stats and tiling. |
| CRS is reduced to a sphere using semi-major axis. | Ellipsoidal input is only approximately represented. | Reject ellipsoidal expectations or implement a full geodetic model. |
| Projection names are exact/narrow. | Semantically equivalent WKT variants may be rejected. | Normalize/inspect CRS operations intentionally. |
| User MPP can disagree with geotransform. | Tile origins and sample spacing disagree. | Resample or reject mismatched overrides. |
| Edge padding is minimum elevation. | Dataset boundary can form an artificial cliff/plateau. | Support edge replication, explicit mask, or clipped tile extents. |
| Processor API returns `void`. | Chunkgen cannot reliably report per-tile failure. | Return structured status and verify every output. |
| Output paths use fixed 256-byte fields. | Long paths can truncate and collide/fail. | Use owned dynamic paths or reject truncation. |
| Existing output skip lives in processor. | Direct processor users can unknowingly retain stale data. | Add content/version fingerprints or explicit overwrite policy. |

### 21.2 Processor and file-format risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Grid coordinates are `int16_t`. | Large effective grids overflow coordinates. | Enforce \(N\) bounds or widen storage. |
| Depth/count math uses integer shifts. | High depths can invoke overflow/undefined behavior. | Validate depth before all shifts; use checked 64-bit math. |
| Activation is signed 8-bit. | Extreme error ratios can overflow activation. | Clamp or widen and validate base error. |
| Propagation is called twice identically. | Extra preprocessing cost; intent uncertain. | Prove/remove with golden mesh and seam tests. |
| Halo indexing is subtle and under-tested. | Tile-edge normals/positions may be wrong under some grids. | Add edge/corner continuity tests before refactoring. |
| Native `.chu` layout. | Cross-endian/ABI portability is undefined. | Version a serialized byte contract. |
| No checksum or payload lengths. | Corruption can become bad allocation/seek/draw state. | Add table, bounds, checksum, and hard reader limits. |
| Loader read checks are incomplete. | Truncated files can produce partially initialized topology. | Check every read and recursive count. |
| Runtime streaming does not check `fopen`, `fseek`, allocations, or `fread`. | Missing/truncated files can crash or upload invalid data after creation. | Make chunk loading transactional and propagate a typed I/O error. |
| Version comparison is not lexicographic. | Some newer/incompatible tuples can be mishandled. | Implement explicit supported-version dispatch. |
| Runtime helper ignores loader failure. | Planet creation may continue with invalid tile data. | Propagate failure and abort transactionally. |

### 21.3 Streaming/cache risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Upload bytes are not checked against staging capacity. | Oversized chunk can overwrite mapped staging memory. | Check before memcpy; chunk/batch or reject. |
| One synchronous upload per frame. | Slow convergence and frame-time I/O spikes. | Add bounded async reads and a byte/time upload budget. |
| Queue is MRU-first and capped at 100. | Starvation/dropped requests under wide views or many tiles. | Use priorities, age/fairness, and dynamic capacity. |
| Roots never evict. | Undersized cache can never achieve base residency. | Preflight aggregate root bytes against both caches. |
| Allocation is 256-byte aligned. | Small chunks create fragmentation. | Measure real wastage; consider size classes/compaction. |
| Separate vertex/index holes must both succeed. | Imbalanced caches can fail despite free space in one pool. | Size from asset statistics or use coordinated budgeting. |
| File is reopened for every upload. | Avoidable filesystem overhead. | Retain handles or memory-map with explicit lifetime. |
| Fallback stats reset per view. | Multi-view telemetry is misleading. | Track per-view and frame aggregate counters. |

### 21.4 LOD/culling risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Error is `baseError * uLevel`. | Quality does not reflect actual per-chunk maximum error; leaves are zero. | Store/consume per-chunk geometric error or document tuned model. |
| Distance can approach zero. | `rho` can become infinity. | Clamp to a small positive distance intentionally. |
| CPU culling casts double bounds/camera path to float. | False culls or unstable transitions at large coordinates. | Use camera-relative doubles for culling. |
| Parent bounds gate all descendants. | Bad parent bounds hide valid children. | Validate recursive containment during asset load/tests. |
| Hysteresis uses residency as state. | Behavior can differ under cache pressure. | Add explicit refinement state if stable transitions matter. |
| Every view traverses independently. | CPU work scales linearly with view count. | Cache only when camera/projection equivalence justifies it. |

### 21.5 Texture/resource risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Old slot clears before new validation. | Failed replacement loses working data. | Build new slices, then atomically swap. |
| Partial tile failures can still return success. | Logical overlay has unexplained gaps. | Return structured per-tile result or all-or-nothing status. |
| Crop files are generated on disk. | I/O cost and possible leftovers/name collisions. | Load pixels directly or own a cleanup manifest. |
| Bindless overflow is assert/TODO. | Large datasets can terminate or corrupt assumptions. | Return capacity errors and expose usage telemetry. |
| One image becomes many resources. | Descriptor/memory use surprises callers. | Preflight and expose estimated slice count/bytes. |
| Composition ignores alpha and adds all five slots. | Colors saturate; “overlay” semantics differ from normal UI textures. | Define blend modes/weights in the shader contract. |
| Default effective overlay strength is 0.09. | Runtime option name alone is misleading. | Remove the extra fixed 0.3 or document it as two-stage gain. |
| Regular tile grid assumed. | Irregular/rotated tile layouts place crops incorrectly. | Intersect against each declared tile rectangle if supported. |

### 21.6 GPU/shader risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Dynamic payload must stay within 256 bytes. | Adding fields can overrun default slot or fail assert. | Calculate layout/alignment and version CPU+GLSL together. |
| High/low macro contains a typo. | “Fixing” one side silently selects wrong layout. | Rename both sides in one tested change, perhaps retain alias. |
| Flatten keeps terrain normals/world output. | Flat geometry still shows relief lighting/custom semantics. | Decide whether flatten means geometry-only or full material flatten. |
| Wireframe depth differs from filled. | Debug image does not represent actual occlusion. | Preserve as diagnostic or align deliberately. |
| Shader contains dead lunar/projection code. | Maintainers may assume it is active; compiler work/noise. | Remove after checking custom shader dependencies. |
| Some GPU object ownership is implicit. | Reload/cleanup leaks can be missed. | Document allocator/manager ownership and test repeated create/destroy. |

### 21.7 dcapp integration risks

| Finding | Consequence | Suggested ownership action |
| --- | --- | --- |
| Model can store multiple PlanetData entries but runtime uses the first. | Authored extra data is silently irrelevant. | Enforce one or define multi-dataset semantics. |
| XML views render to fixed 1024 square. | Quality/aspect cost can mismatch displayed size. | Create/resize output from desired resolution policy. |
| Snapshot duplicates metadata parser. | Schema behavior can drift. | Extract shared metadata loading. |
| Legacy projection path has compatibility flips. | Old/new texture placement differs subtly. | Migrate assets and retire legacy path with golden images. |
| Stable handle index is 8-bit. | Large dynamic object counts exhaust silently or wrap if unchecked. | Widen or hard-fail at capacity. |
| Shared planet has shared light/textures/cache. | One view cannot independently override these. | Move state to view only if that semantic is required. |
| Load flags `DEBUG`/`CACHE_TEXTURES` have little or no distinct runtime effect. | API suggests behavior that code does not deliver. | Implement or remove/deprecate them. |

## 22. Safe modification recipes

### 22.1 Adding a projection

This is a cross-pipeline change. Touching only one conversion will produce
geometry that loads but does not align with runtime textures or APIs.

1. Add the enum/parameter record in
   [`extensions/pl_planet_processor_ext.h`](extensions/pl_planet_processor_ext.h).
2. Teach chunkgen to recognize GDAL CRS metadata, normalize units, and serialize
   every required parameter.
3. Implement projected-grid-to-geodetic inverse in the processor.
4. Implement geodetic-to-projected forward in [`src/geo.c`](src/geo.c) and call
   it from texture placement.
5. Extend `.planet.json` writer and dcapp parser.
6. Extend snapshot's duplicate parser or, preferably, remove the duplication.
7. Define legacy/version behavior; do not infer projection from missing fields
   for new assets.
8. Add known control points, forward/inverse round trips, tile-boundary tests,
   overlay alignment snapshots, and both hemispheres/origin offsets.

### 22.2 Changing `.chu`

1. Give the revision an explicit reader branch.
2. Update processor writer and loader together.
3. Update runtime seek/skip code in `prepare()`; it independently interprets
   the serialized chunk prefix.
4. Keep old golden files and verify load/render compatibility.
5. Validate file sizes/counts before allocation.
6. Update this document's byte layout and version notes.

The runtime payload reader is easy to overlook because the topology loader has
already scanned the same file. A header change can let topology creation appear
to work while streaming seeks to the wrong payload bytes.

### 22.3 Changing vertex format

Update as one atomic contract:

- processor vertex structs and writer,
- runtime `szVertexSize`, fread size, and vertex-buffer offsets,
- all four pipeline vertex layouts,
- shader locations and high/low macro variants,
- `.chu` version/flags,
- staging capacity calculations,
- custom/sample shaders.

Add static size/offset assertions on the C structs. A vertex-layout mismatch
usually presents as exploding geometry, incorrect normals, or UV corruption,
not a clean error.

### 22.4 Changing dynamic shader data

1. Modify `pl_shader_interop_planet.h`, not independent duplicate structs.
2. Account for C/GLSL alignment, especially `vec3`, arrays, and matrices.
3. Keep payload at or below the backend dynamic slot size or increase the slot
   contract everywhere.
4. Populate the field for every draw path and pipeline variant.
5. Rebuild default and custom shaders; test hot reload.

### 22.5 Adding an overlay slot

The count is baked into more than the public constant. Update:

- public runtime and dcapp constants,
- per-tile path/resource/index arrays,
- UV allocation/index arithmetic,
- texture clear/set loops and XML declaration parsing,
- dynamic data texture indices and transforms,
- fragment sampling/composition,
- logic header generator/types/sample,
- bindless capacity estimates and tests.

Consider replacing individually named dynamic fields with a carefully aligned
array, but verify shader-layout portability first.

### 22.6 Changing LOD policy

Preserve these correctness properties unless the replacement explicitly solves
them another way:

- a tile root can eventually render,
- a parent remains drawable until all replacement children are resident,
- no parent is evicted while resident descendants depend on it,
- missing chunks are continuously re-requested but do not starve permanently,
- refinement does not introduce cracks or empty quadrants,
- flatten uses matching culling bounds,
- multiple views do not corrupt shared state.

For a real screen-space-error system, serialize per-chunk maximum deviation,
project it with pixel focal length, and test threshold invariance across output
resolution/FOV. Do not silently reinterpret existing `max_base_error` files.

### 22.7 Making streaming asynchronous

A robust design needs distinct states rather than `ptIndexHole == NULL` doing
most of the work:

```text
unrequested -> queued -> reading -> ready-for-upload -> resident
                         \-> failed/retry
```

File workers must not mutate GPU freelists or chunk pointers without ownership
synchronization. Reserve/commit cache space deliberately, cap bytes and time per
frame, and ensure a canceled/evicted request cannot upload into a reassigned
hole. Keep command-buffer/staging regions alive until GPU completion.

### 22.8 Resizing views

Today a view's output resources are created once. Dynamic resizing must recreate
color/depth textures and the composite bind group in a GPU-safe deferred manner,
then update the viewport/scissor and any resolution-dependent LOD expectations.
Logical display dimensions and framebuffer dimensions are separate inputs; do
not conflate them when choosing aspect or render resolution.

## 23. Debugging and validation playbook

### 23.1 Establish the asset facts first

Use GDAL tooling to inspect, while remembering chunkgen calls equivalent C APIs:

```sh
gdalinfo input.tif
```

Record:

- raster width/height and band count,
- CRS projection method and sphere/semi-major axis,
- latitude of origin/standard parallel, central meridian, scale factor, false
  easting/northing,
- affine origin and pixel size,
- linear units,
- band type, scale, offset, NoData, mask, and min/max.

Then run chunkgen with `--keep-tiles`. Verify the emitted JSON centers using the
affine equations and inspect UInt16 edge tiles for expected padding. Generated
logs print every normalized projection parameter used by the processor.

### 23.2 Check coordinate control points

At minimum test:

- the projection pole maps to `(false_easting,false_northing)`,
- central-meridian points have expected X sign/zero,
- known projected points round-trip through inverse then forward,
- Cartesian longitude zero lies on +Z,
- east-positive longitude moves toward +X,
- geodetic texture center lands on the corresponding terrain feature.

Use double tolerances in projected meters appropriate to the input resolution,
not exact equality after trigonometry.

### 23.3 Check seams

Generate a synthetic multi-tile DEM with analytical height, for example

\[
h(x,y)=a+bx+cy+d\sin(kx)\cos(ky).
\]

For each shared edge and four-tile corner compare:

- decoded heights,
- emitted Cartesian high+low positions,
- decoded normals,
- chunk bounds containment,
- rendered wireframe continuity at every LOD transition.

A planar ramp makes sign and row-direction errors obvious; a sinusoid exercises
normal continuity.

### 23.4 Runtime visual flags

- **Wireframe:** inspect triangulation and chunk replacement.
- **Show levels:** inspect LOD rings/transitions.
- **Show chunks:** distinguish neighboring draw units.
- **Show origin:** displays the global transform axes at 1.2 radii.
- **Flatten:** separates elevation geometry from base-sphere placement.

Combine stream statistics with these modes. A persistent coarse colored parent
with nonzero fallback means children are requested/not all resident. Pending
zero plus fallback nonzero indicates a state/policy inconsistency worth tracing.

### 23.5 Snapshot as a regression oracle

Use fixed assets/cameras and store images or perceptual hashes for:

- default filled output,
- flattened output,
- level/chunk debug views,
- north/south projection cases,
- each overlay slot and all slots together,
- custom elevation/slope fragment shaders,
- views near a tile seam and four-tile corner.

Snapshot waits for request/fallback settlement, removing much of the timing
noise from normal interactive captures. Also record resident/total/frame counts;
an image can remain visually similar while streaming behavior regresses.

### 23.6 Cache-pressure tests

Generate or choose an asset with known root/child byte totals. Test:

1. cache comfortably larger than the working set,
2. cache just above aggregate root requirement,
3. one cache side constrained more than the other,
4. cache too small for roots and verify graceful failure after fixes,
5. multiple opposing views that churn different regions,
6. more than 100 simultaneous missing visible chunks.

Track freelist free bytes **and largest hole**, request age, dropped/reassigned
requests, upload bytes/time, resident levels, and evictions. Current public
stats expose only part of this; temporary instrumentation in the extension is
often necessary.

### 23.7 GPU validation

When changing GPU code, verify:

- single and high/low layouts,
- filled and wireframe variants,
- custom and default shaders,
- reverse-Z terrain/annotation occlusion,
- repeated view create/destroy and shader reload,
- all frames-in-flight after bindless descriptor update/reuse,
- staging-bound checks with a deliberately oversized chunk,
- device/backend validation output on every supported graphics backend.

### 23.8 Build and smoke-test sequence

For documentation-only changes no binary rebuild is needed. For implementation
changes, a practical sequence is:

```sh
make build
./bin/dcapp-planet-chunkgen.sh test.tif test-output --keep-tiles
./bin/dcapp-planet-snapshot.sh \
  --planet-data test-output/test.planet.json \
  --crs geodetic --attitude-frame local-ned \
  --lat -70 --lon 0 --elevation 500000 \
  --output test-output/smoke.png
```

Then run the planet sample and exercise texture refresh, all five slots, shader
switching, tau, flatten, orthographic/perspective, logic-owned and XML-owned
views, GeoJSON, breadcrumbs, local geometry, and labels.

## 24. Maintainer checklist

### Before accepting a new DEM

- Confirm its CRS is spherical polar stereographic under current support.
- Confirm projected units and geotransform are meters-equivalent, square, and
  unrotated.
- Confirm elevation scale, offset, NoData, and mask semantics.
- Choose height bounds deliberately; outliers determine quantization resolution.
- Confirm tile size/tree depth remain within processor coordinate/shift limits.
- Estimate root and expected working-set bytes before choosing cache size.
- Generate with kept tiles once and inspect edge/corner behavior.
- Render fixed snapshots with overlay control points.

### Before changing processor math

- Preserve the \(2^L+1\) interval/grid relationship.
- Test projection forward/inverse as a pair.
- Test positions **and normals** on all seams.
- Verify BTT mate constraints and no T-junctions.
- Recompute curved/flat bounds and recursive containment.
- Version any serialized result whose interpretation changes.
- Test old and new `.chu` readers explicitly.

### Before changing runtime streaming or LOD

- Preflight roots and staging payload sizes.
- Preserve parent fallback until all four children are ready.
- Preserve bottom-up eviction safety.
- Exercise queue saturation and fairness.
- Test two views sharing one planet.
- Check both curve and flatten bounds.
- Compare convergence frames and frame-time spikes, not only final images.

### Before changing GPU/shaders

- Keep CPU structs, shader interop, pipeline layouts, and vertex strides in sync.
- Respect the 256-byte dynamic-data boundary/alignment.
- Test all four pipeline variants.
- Preserve reverse-Z as a complete projection/clear/compare/depth-write contract.
- Check camera-relative precision at the largest supported radius/altitude.
- Decide explicitly how flatten affects position, normals, bounds, and custom
  shader outputs.
- Validate bindless updates for every frame in flight.
- Run repeated creation, reload, and cleanup under GPU validation.

### Before changing dcapp integration

- Test XML-owned and logic-owned objects.
- Keep view-before-planet and planet-before-resource-manager teardown.
- Preserve frame order: prepare, traverse/request, offscreen render, composite.
- Check fixed versus requested view resolution and logical aspect separately.
- Update metadata parsing in both adapter and snapshot until they are shared.
- Update generated logic headers/sample code for exposed constants or types.
- Retire legacy coordinate behavior only with asset migration and golden images.

## Closing mental model

The simplest correct way to reason about this system is:

- GDAL defines what every source pixel means in projected meters.
- The processor turns those meters into a spherical, independently drawable
  hierarchy and bakes nearly all geometry work offline.
- The runtime never regenerates terrain. It scans topology, streams opaque mesh
  payloads into two GPU pools, and chooses which already-baked chunks to draw.
- A parent is the safety net until all four children are resident.
- High/low floats plus camera-relative subtraction make planet-scale coordinates
  renderable without GPU doubles.
- Runtime images are split per terrain tile, indexed bindlessly, and mapped into
  each chunk through precomputed affine UV transforms.
- dcapp is the owner of object identity, coordinate/API translation, cameras,
  annotations, and frame/lifetime ordering; the extension is the owner of
  terrain residency and rendering.

If a change crosses any of those boundaries—projection, serialization, vertex
layout, dynamic shader data, LOD semantics, or lifetime—treat it as a pipeline
change and test both offline generation and runtime rendering together.
