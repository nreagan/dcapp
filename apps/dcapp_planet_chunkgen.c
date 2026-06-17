/*
   dcapp_planet_chunkgen.c

   Pilotlight app that builds a .planet.json manifest and .p2c payload chunks
   from one or more GDAL-readable DEMs. It creates a source catalog, sparse
   cube-sphere tile records, and GPU-ready tile meshes in Pilotlight's
   Cartesian frame.

   Usage:
     pilot_light -a dcapp-planet-chunkgen <output_dir> <dem...> [options]
*/

/*
Index of this file:
// [SECTION] includes
// [SECTION] structs
// [SECTION] forward declarations
// [SECTION] extension globals
// [SECTION] pl_app_load
// [SECTION] helpers
// [SECTION] GDAL helpers
// [SECTION] JSON writing
*/

//-----------------------------------------------------------------------------
// [SECTION] includes
//-----------------------------------------------------------------------------

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdal.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>

#include "pl.h"
#include "pl_planet_ext.h"
#include "../src/utils/file.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

#define DC_PLANET_MAX_DEMS 256u
#define DC_PLANET_FOOTPRINT_SAMPLES 33u
#define DC_PLANET_TILE_SOURCE_SAMPLE_GRID 7u
#define DC_PLANET_DEFAULT_MAX_WINDOW_MB 256u

//-----------------------------------------------------------------------------
// [SECTION] structs
//-----------------------------------------------------------------------------

typedef enum _DcPlanetDemProjectionType
{
    DC_PLANET_DEM_PROJECTION_POLAR_STEREOGRAPHIC,
    DC_PLANET_DEM_PROJECTION_CYLINDRICAL
} DcPlanetDemProjectionType;

typedef enum _DcPlanetHeightMode
{
    DC_PLANET_HEIGHT_MODE_AUTO,
    DC_PLANET_HEIGHT_MODE_RADIAL,
    DC_PLANET_HEIGHT_MODE_OFFSET
} DcPlanetHeightMode;

typedef struct _DcPlanetDemInput
{
    char    path[DC_UTILS_FILEPATH_BUFFER_SIZE];
    int32_t priority;
} DcPlanetDemInput;

typedef struct _DcPlanetDemInfo
{
    DcPlanetDemProjectionType projection_type;
    plPlanetSourceRecord      source;

    uint32_t width;
    uint32_t height;
    double   radius;
    double   gt[6];
    double   to_meters;
    double   band_scale;
    double   band_offset;

    struct
    {
        double latitude_of_origin;
        double longitude_of_origin;
        double scale_factor;
        double false_easting;
        double false_northing;
    } polar;

    struct
    {
        bool   angular;
        double angular_to_degrees;
        double center_latitude;
        double center_longitude;
        double standard_parallel;
        double false_easting;
        double false_northing;
    } cylindrical;
} DcPlanetDemInfo;

typedef struct _DcPlanetDemRuntime
{
    GDALDatasetH                 dataset;
    GDALRasterBandH              band;
    OGRSpatialReferenceH         source_srs;
    OGRSpatialReferenceH         geodetic_srs;
    OGRCoordinateTransformationH geodetic_to_source;
    double                       inv_gt[6];
    int                          has_nodata;
    double                       nodata;
} DcPlanetDemRuntime;

typedef struct _DcPlanetSourceWindow
{
    int    xoff;
    int    yoff;
    int    width;
    int    height;
    float *pixels;
} DcPlanetSourceWindow;

//-----------------------------------------------------------------------------
// [SECTION] forward declarations
//-----------------------------------------------------------------------------

static void _show_help(void);
static bool _parse_source_arg(const char *arg, DcPlanetDemInput *out_input);
static bool _parse_height_mode(const char *arg, DcPlanetHeightMode *out_mode);
static bool _read_dem_metadata(const DcPlanetDemInput *input, double radius_override, DcPlanetDemInfo *out_info);
static bool _open_dem_runtimes(const DcPlanetDemInfo *dem_infos, uint32_t dem_count, DcPlanetDemRuntime *runtimes);
static void _close_dem_runtimes(DcPlanetDemRuntime *runtimes, uint32_t dem_count);
static bool _choose_tile_primary_source(const plPlanetManifest *manifest, plPlanetFace face, uint8_t lod, uint32_t x, uint32_t y, uint32_t *out_source_index);
static bool _source_record_contains(const plPlanetSourceRecord *source, double latitude, double longitude);
static bool _source_latlon_to_pixel(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, double latitude, double longitude, double *out_pixel_x, double *out_pixel_y);
static bool _accept_pixel_for_sampling(const DcPlanetDemInfo *info, double pixel_x, double pixel_y, double *out_pixel_x, double *out_pixel_y);
static bool _choose_sample_source(const DcPlanetDemInfo *dem_infos, const DcPlanetDemRuntime *runtimes, uint32_t dem_count, double latitude, double longitude, uint32_t *out_source_index, double *out_pixel_x, double *out_pixel_y);
static bool _pixel_to_geodetic(const DcPlanetDemInfo *info, double pixel_x, double pixel_y, double *out_lat, double *out_lon);
static bool _accumulate_footprint_sample(const DcPlanetDemInfo *info, double pixel_x, double pixel_y, double *min_lat, double *max_lat, double *lons, uint32_t *lon_count);
static bool _compute_footprint(DcPlanetDemInfo *info);
static bool _write_planet_chunk(const char *output_dir, plPlanetTileRecord *tile, const DcPlanetDemInfo *dem_infos, const DcPlanetDemRuntime *runtimes, uint32_t dem_count, double radius, uint32_t tile_size, DcPlanetHeightMode height_mode, uint32_t max_window_mb);
static bool _read_source_window(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, int xoff, int yoff, int width, int height, DcPlanetSourceWindow *out_window);
static bool _sample_source_height(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, const DcPlanetSourceWindow *window, double pixel_x, double pixel_y, double *out_height);
static bool _source_height_is_radial(const DcPlanetDemInfo *info, double radius, DcPlanetHeightMode height_mode);
static double _height_to_radial_distance(const DcPlanetDemInfo *info, double radius, double height, DcPlanetHeightMode height_mode);
static double _height_to_elevation_offset(const DcPlanetDemInfo *info, double radius, double height, DcPlanetHeightMode height_mode);
static plVec3d _face_uv_to_direction_unclamped(plPlanetFace face, double u, double v);
static plVec3d _geodetic_to_direction(double latitude, double longitude);
static plVec3d _sample_planet_position_at_face_uv(plPlanetFace face, double face_u, double face_v, const DcPlanetDemInfo *dem_infos, const DcPlanetDemRuntime *runtimes, const DcPlanetSourceWindow *windows, uint32_t dem_count, double radius, DcPlanetHeightMode height_mode);
static bool _sample_planet_position_at_source_pixel(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, const DcPlanetSourceWindow *window, double pixel_x, double pixel_y, double radius, DcPlanetHeightMode height_mode, plVec3d *out_position);
static void _write_skirt_vertex(plPlanetVertex *vertices, uint32_t dst_index, const plPlanetVertex *src_vertex, plVec3d src_position, double skirt_depth, plVec3d *min_bound, plVec3d *max_bound);
static void _add_skirt_quad_indices(uint32_t *indices, uint32_t *cursor, uint32_t edge0, uint32_t edge1, uint32_t skirt0, uint32_t skirt1);
static bool _write_manifest_json(const char *output_dir, const char *prefix, const plPlanetManifest *manifest, const DcPlanetDemInfo *dem_infos, uint32_t dem_count, double radius, uint32_t tile_size, uint8_t max_lod, bool write_payloads, DcPlanetHeightMode height_mode);
static const char *_face_name(plPlanetFace face);
static char *_get_stem(const char *path, char *buf, size_t buf_size);
static double _estimate_tile_geometric_error(double radius, double source_mpp, uint32_t tile_size, uint8_t lod);
static double _normalize_lon(double lon);
static double _angle_units_to_degrees(OGRSpatialReferenceH srs);
static bool _is_equirectangular_projection(const char *projection_name);
static bool _read_cylindrical_projection(OGRSpatialReferenceH srs, bool angular, DcPlanetDemInfo *out_info);
static bool _cylindrical_latlon_to_pixel(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, double latitude, double longitude, double *out_pixel_x, double *out_pixel_y);
static void _planet_split_double(double value, float *high_out, float *low_out);
static plVec3d _mul_vec3d_scalar(plVec3d v, double s);
static int _compare_double(const void *a, const void *b);

//-----------------------------------------------------------------------------
// [SECTION] extension globals
//-----------------------------------------------------------------------------

static const plIOI      *_ext_ioi     = NULL;
static const plPlanetI *_ext_planet = NULL;

//-----------------------------------------------------------------------------
// [SECTION] pl_app_load
//-----------------------------------------------------------------------------

PL_EXPORT void *pl_app_load(plApiRegistryI *api_registry, void *app_data) {
    if (app_data)
        return app_data;

    const plExtensionRegistryI *extension_registry = pl_get_api_latest(api_registry, plExtensionRegistryI);
    extension_registry->load("pl_unity_ext", NULL, NULL, true);
    extension_registry->load("pl_platform_ext", "pl_load_platform_ext", "pl_unload_platform_ext", false);
    extension_registry->load("pl_planet_ext", NULL, NULL, true);

    _ext_ioi = pl_get_api_latest(api_registry, plIOI);
    _ext_planet = pl_get_api_latest(api_registry, plPlanetI);
    plIO *io = _ext_ioi->get_io();
    if (!_ext_planet) {
        fprintf(stderr, "Error: failed to get plPlanetI from registry\n");
        io->bRunning = false;
        return NULL;
    }

    int argc = io->iArgc - 3;
    char **argv = io->apArgv + 3;

    const char *output_dir = NULL;
    const char *prefix = "planet";
    double radius_override = 0.0;
    uint32_t tile_size = 257;
    uint8_t max_lod = 4;
    bool write_payloads = true;
    DcPlanetHeightMode height_mode = DC_PLANET_HEIGHT_MODE_AUTO;
    uint32_t max_window_mb = DC_PLANET_DEFAULT_MAX_WINDOW_MB;
    DcPlanetDemInput dem_inputs[DC_PLANET_MAX_DEMS] = {0};
    uint32_t dem_count = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "--planet-help") == 0) {
            _show_help();
            io->bRunning = false;
            return NULL;
        } else if (strcmp(argv[i], "--radius") == 0 && i + 1 < argc) {
            radius_override = atof(argv[++i]);
        } else if (strcmp(argv[i], "--tile-size") == 0 && i + 1 < argc) {
            tile_size = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-lod") == 0 && i + 1 < argc) {
            int value = atoi(argv[++i]);
            if (value < 0 || value > 12) {
                fprintf(stderr, "Error: --max-lod must be in [0, 12]\n");
                io->bRunning = false;
                return NULL;
            }
            max_lod = (uint8_t)value;
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--manifest-only") == 0) {
            write_payloads = false;
        } else if (strcmp(argv[i], "--height-mode") == 0 && i + 1 < argc) {
            if (!_parse_height_mode(argv[++i], &height_mode)) {
                io->bRunning = false;
                return NULL;
            }
        } else if (strcmp(argv[i], "--max-window-mb") == 0 && i + 1 < argc) {
            int value = atoi(argv[++i]);
            if (value < 1) {
                fprintf(stderr, "Error: --max-window-mb must be positive\n");
                io->bRunning = false;
                return NULL;
            }
            max_window_mb = (uint32_t)value;
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            if (dem_count >= DC_PLANET_MAX_DEMS) {
                fprintf(stderr, "Error: too many DEM sources\n");
                io->bRunning = false;
                return NULL;
            }
            if (!_parse_source_arg(argv[++i], &dem_inputs[dem_count++])) {
                io->bRunning = false;
                return NULL;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            io->bRunning = false;
            return NULL;
        } else if (!output_dir) {
            output_dir = argv[i];
        } else {
            if (dem_count >= DC_PLANET_MAX_DEMS) {
                fprintf(stderr, "Error: too many DEM sources\n");
                io->bRunning = false;
                return NULL;
            }
            if (!_parse_source_arg(argv[i], &dem_inputs[dem_count++])) {
                io->bRunning = false;
                return NULL;
            }
        }
    }

    if (!output_dir || dem_count == 0) {
        fprintf(stderr, "Error: output_dir and at least one DEM source are required\n");
        _show_help();
        io->bRunning = false;
        return NULL;
    }

    if (tile_size < 2) {
        fprintf(stderr, "Error: --tile-size must be at least 2\n");
        io->bRunning = false;
        return NULL;
    }

    if (dc_utils_create_directory(output_dir) != 0) {
        io->bRunning = false;
        return NULL;
    }

    GDALAllRegister();
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");

    printf("========================================\n");
    printf("dcapp-planet-chunkgen\n");
    printf("========================================\n");
    printf("Output: %s\n", output_dir);
    printf("Prefix: %s\n", prefix);
    printf("Sources: %u\n", dem_count);
    printf("Payloads: %s\n", write_payloads ? "yes" : "manifest only");

    DcPlanetDemInfo *dem_infos = (DcPlanetDemInfo *)calloc(dem_count, sizeof(DcPlanetDemInfo));
    if (!dem_infos) {
        fprintf(stderr, "Error: failed to allocate DEM metadata\n");
        io->bRunning = false;
        return NULL;
    }

    double radius = radius_override;
    for (uint32_t i = 0; i < dem_count; i++) {
        if (!dc_utils_file_exists(dem_inputs[i].path)) {
            fprintf(stderr, "Error: DEM source not found: %s\n", dem_inputs[i].path);
            free(dem_infos);
            io->bRunning = false;
            return NULL;
        }
        if (!_read_dem_metadata(&dem_inputs[i], radius_override, &dem_infos[i])) {
            free(dem_infos);
            io->bRunning = false;
            return NULL;
        }
        if (radius <= 0.0 && dem_infos[i].radius > 0.0)
            radius = dem_infos[i].radius;
    }

    if (radius <= 0.0) {
        fprintf(stderr, "Error: could not detect planet radius; use --radius\n");
        free(dem_infos);
        io->bRunning = false;
        return NULL;
    }

    plPlanetManifestInit manifest_init = {
        .dRadius = radius,
        .uTileSize = tile_size,
        .uMaxLod = max_lod,
        .tTilingMode = PL_PLANET_TILING_CUBE_SPHERE,
        .tFlags = PL_PLANET_MANIFEST_FLAGS_NONE
    };
    plPlanetManifest *manifest = _ext_planet->create_manifest(manifest_init);
    if (!manifest) {
        fprintf(stderr, "Error: failed to create planet manifest\n");
        free(dem_infos);
        io->bRunning = false;
        return NULL;
    }

    for (uint32_t i = 0; i < dem_count; i++) {
        uint32_t source_index = _ext_planet->add_source(manifest, dem_infos[i].source);
        if (source_index == UINT32_MAX) {
            fprintf(stderr, "Error: failed to add source: %s\n", dem_infos[i].source.acPath);
            _ext_planet->cleanup_manifest(manifest);
            free(dem_infos);
            io->bRunning = false;
            return NULL;
        }
        printf("Source %u: %s mpp=%.3f priority=%d lat=[%.3f, %.3f] lon=[%.3f, %.3f]\n",
               source_index,
               dem_infos[i].source.acName,
               dem_infos[i].source.dNativeMetersPerPixel,
               dem_infos[i].source.uPriority,
               dem_infos[i].source.dMinLatitude,
               dem_infos[i].source.dMaxLatitude,
               dem_infos[i].source.dMinLongitude,
               dem_infos[i].source.dMaxLongitude);
    }

    DcPlanetDemRuntime *dem_runtimes = NULL;
    if (write_payloads) {
        dem_runtimes = (DcPlanetDemRuntime *)calloc(dem_count, sizeof(DcPlanetDemRuntime));
        if (!dem_runtimes) {
            fprintf(stderr, "Error: failed to allocate DEM runtime handles\n");
            _ext_planet->cleanup_manifest(manifest);
            free(dem_infos);
            io->bRunning = false;
            return NULL;
        }
        if (!_open_dem_runtimes(dem_infos, dem_count, dem_runtimes)) {
            _close_dem_runtimes(dem_runtimes, dem_count);
            free(dem_runtimes);
            _ext_planet->cleanup_manifest(manifest);
            free(dem_infos);
            io->bRunning = false;
            return NULL;
        }
    }

    printf("Generating cube-sphere tile %s through LOD %u...\n", write_payloads ? "chunks" : "manifest", max_lod);
    uint32_t tile_records = 0;
    for (uint8_t lod = 0; lod <= max_lod; lod++) {
        const uint32_t dim = 1u << lod;
        for (int face = 0; face < PL_PLANET_FACE_COUNT; face++) {
            for (uint32_t y = 0; y < dim; y++) {
                for (uint32_t x = 0; x < dim; x++) {
                    uint32_t source_index = UINT32_MAX;
                    if (!_choose_tile_primary_source(manifest, face, lod, x, y, &source_index))
                        continue;

                    const plPlanetSourceRecord *source = _ext_planet->get_source(manifest, source_index);
                    if (!source)
                        continue;

                    plPlanetTileRecord tile = {0};
                    tile.tAddress = _ext_planet->make_address(face, lod, x, y);
                    tile.uSourceIndex = source_index;
                    tile.dGeometricError = _estimate_tile_geometric_error(radius, source->dNativeMetersPerPixel, tile_size, lod);
                    tile.dMinHeight = source->dMinHeight;
                    tile.dMaxHeight = source->dMaxHeight;
                    snprintf(tile.acChunkFile, sizeof(tile.acChunkFile), "%s_f%d_l%u_%u_%u.p2c", prefix, face, lod, x, y);
                    if (write_payloads) {
                        if (!_write_planet_chunk(output_dir, &tile, dem_infos, dem_runtimes, dem_count, radius, tile_size, height_mode, max_window_mb)) {
                            fprintf(stderr, "Error: failed to write tile payload face=%d lod=%u x=%u y=%u\n", face, lod, x, y);
                            _close_dem_runtimes(dem_runtimes, dem_count);
                            free(dem_runtimes);
                            _ext_planet->cleanup_manifest(manifest);
                            free(dem_infos);
                            io->bRunning = false;
                            return NULL;
                        }
                    }
                    if (!_ext_planet->add_tile(manifest, tile)) {
                        fprintf(stderr, "Error: failed to add tile record face=%d lod=%u x=%u y=%u\n", face, lod, x, y);
                        if (dem_runtimes) {
                            _close_dem_runtimes(dem_runtimes, dem_count);
                            free(dem_runtimes);
                        }
                        _ext_planet->cleanup_manifest(manifest);
                        free(dem_infos);
                        io->bRunning = false;
                        return NULL;
                    }
                    tile_records++;
                }
            }
        }
    }

    _ext_planet->finalize_manifest(manifest);
    printf("Tile records: %u\n", tile_records);

    if (!_write_manifest_json(output_dir, prefix, manifest, dem_infos, dem_count, radius, tile_size, max_lod, write_payloads, height_mode)) {
        if (dem_runtimes) {
            _close_dem_runtimes(dem_runtimes, dem_count);
            free(dem_runtimes);
        }
        _ext_planet->cleanup_manifest(manifest);
        free(dem_infos);
        io->bRunning = false;
        return NULL;
    }

    if (dem_runtimes) {
        _close_dem_runtimes(dem_runtimes, dem_count);
        free(dem_runtimes);
    }
    _ext_planet->cleanup_manifest(manifest);
    free(dem_infos);

    printf("Done.\n");
    io->bRunning = false;
    return NULL;
}

PL_EXPORT void pl_app_shutdown(void *app_data) {
    (void)app_data;
}

PL_EXPORT void pl_app_resize(plWindow *window, void *app_data) {
    (void)window;
    (void)app_data;
}

PL_EXPORT void pl_app_update(void *app_data) {
    (void)app_data;
}

//-----------------------------------------------------------------------------
// [SECTION] helpers
//-----------------------------------------------------------------------------

static void _show_help(void) {
    printf("dcapp-planet-chunkgen - Build a planet multi-DEM manifest\n\n");
    printf("Usage:\n");
    printf("  dcapp-planet-chunkgen <output_dir> <dem...> [options]\n\n");
    printf("Options:\n");
    printf("  --source PATH[,PRIORITY]  Add DEM source with optional explicit priority\n");
    printf("  --radius N               Override planet radius in meters\n");
    printf("  --tile-size N            Output tile vertex dimension hint (default: 257)\n");
    printf("  --max-lod N              Highest cube-sphere manifest LOD, 0-12 (default: 4)\n");
    printf("  --prefix NAME            Manifest and future payload prefix (default: planet)\n");
    printf("  --manifest-only          Write .planet.json without .p2c payload chunks\n");
    printf("  --height-mode MODE       auto, radial, or offset (default: auto)\n");
    printf("  --max-window-mb N        Max per-source tile cache before direct sampling (default: 256)\n");
    printf("  --planet-help           Show this help without invoking Pilotlight help\n");
    printf("  -h, --help               Show this help\n\n");
    printf("Notes:\n");
    printf("  Supported source CRS families: polar stereographic and cylindrical/\n");
    printf("  equirectangular lon-lat or projected rasters.\n");
    printf("  Payload chunks use Pilotlight Cartesian positions and keep source CRS math in\n");
    printf("  chunk generation, not runtime rendering.\n");
}

static bool _parse_source_arg(const char *arg, DcPlanetDemInput *out_input) {
    if (!arg || !out_input || arg[0] == '\0')
        return false;

    memset(out_input, 0, sizeof(*out_input));
    out_input->priority = 0;

    const char *comma = strrchr(arg, ',');
    if (comma && comma[1] != '\0') {
        bool numeric = true;
        const char *p = comma + 1;
        if (*p == '-' || *p == '+')
            p++;
        while (*p) {
            if (!isdigit((unsigned char)*p)) {
                numeric = false;
                break;
            }
            p++;
        }
        if (numeric) {
            size_t path_len = (size_t)(comma - arg);
            if (path_len >= sizeof(out_input->path)) {
                fprintf(stderr, "Error: source path too long: %s\n", arg);
                return false;
            }
            memcpy(out_input->path, arg, path_len);
            out_input->path[path_len] = '\0';
            out_input->priority = atoi(comma + 1);
            return true;
        }
    }

    strncpy(out_input->path, arg, sizeof(out_input->path) - 1);
    return true;
}

static bool _parse_height_mode(const char *arg, DcPlanetHeightMode *out_mode) {
    if (!arg || !out_mode)
        return false;
    if (strcmp(arg, "auto") == 0) {
        *out_mode = DC_PLANET_HEIGHT_MODE_AUTO;
        return true;
    }
    if (strcmp(arg, "radial") == 0) {
        *out_mode = DC_PLANET_HEIGHT_MODE_RADIAL;
        return true;
    }
    if (strcmp(arg, "offset") == 0) {
        *out_mode = DC_PLANET_HEIGHT_MODE_OFFSET;
        return true;
    }
    fprintf(stderr, "Error: --height-mode must be auto, radial, or offset\n");
    return false;
}

static char *_get_stem(const char *path, char *buf, size_t buf_size) {
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!base || (backslash && backslash > base))
        base = backslash;
#endif
    base = base ? base + 1 : path;

    strncpy(buf, base, buf_size - 1);
    buf[buf_size - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot)
        *dot = '\0';
    return buf;
}

static double _estimate_tile_geometric_error(double radius, double source_mpp, uint32_t tile_size, uint8_t lod) {
    if (radius <= 0.0 || tile_size < 2)
        return source_mpp > 0.0 ? source_mpp * 0.5 : 1.0;

    const uint32_t dim = 1u << lod;
    const double face_span_m = radius * M_PI_2;
    const double mesh_mpp = face_span_m / ((double)dim * (double)(tile_size - 1u));
    double limiting_mpp = mesh_mpp;
    if (source_mpp > limiting_mpp)
        limiting_mpp = source_mpp;
    return limiting_mpp > 0.0 ? limiting_mpp * 0.5 : 1.0;
}

static const char *_face_name(plPlanetFace face) {
    switch (face) {
        case PL_PLANET_FACE_POS_X: return "+X";
        case PL_PLANET_FACE_NEG_X: return "-X";
        case PL_PLANET_FACE_POS_Y: return "+Y";
        case PL_PLANET_FACE_NEG_Y: return "-Y";
        case PL_PLANET_FACE_POS_Z: return "+Z";
        case PL_PLANET_FACE_NEG_Z: return "-Z";
        default: return "?";
    }
}

static double _normalize_lon(double lon) {
    double out = fmod(lon, 360.0);
    if (out < 0.0)
        out += 360.0;
    return out;
}

static double _angle_units_to_degrees(OGRSpatialReferenceH srs) {
    double radians_per_unit = OSRGetAngularUnits(srs, NULL);
    if (radians_per_unit <= 0.0)
        return 1.0;
    return radians_per_unit * 180.0 / M_PI;
}

static bool _is_equirectangular_projection(const char *projection_name) {
    return projection_name &&
        (strcmp(projection_name, SRS_PT_EQUIRECTANGULAR) == 0 ||
         strcmp(projection_name, "Equidistant_Cylindrical") == 0 ||
         strcmp(projection_name, "Plate_Carree") == 0 ||
         strcmp(projection_name, "Simple_Cylindrical") == 0);
}

static bool _read_cylindrical_projection(OGRSpatialReferenceH srs, bool angular, DcPlanetDemInfo *out_info) {
    out_info->projection_type = DC_PLANET_DEM_PROJECTION_CYLINDRICAL;
    out_info->cylindrical.angular = angular;
    out_info->cylindrical.angular_to_degrees = angular ? _angle_units_to_degrees(srs) : 1.0;
    if (out_info->cylindrical.angular_to_degrees <= 0.0)
        out_info->cylindrical.angular_to_degrees = 1.0;

    if (angular) {
        out_info->to_meters = 1.0;
        out_info->cylindrical.center_latitude = 0.0;
        out_info->cylindrical.center_longitude = 0.0;
        out_info->cylindrical.standard_parallel = 0.0;
        out_info->cylindrical.false_easting = 0.0;
        out_info->cylindrical.false_northing = 0.0;
        return true;
    }

    out_info->to_meters = OSRGetLinearUnits(srs, NULL);
    if (out_info->to_meters <= 0.0)
        out_info->to_meters = 1.0;

    OGRErr lat_err = OGRERR_NONE;
    double center_lat = OSRGetProjParm(srs, SRS_PP_LATITUDE_OF_ORIGIN, 0.0, &lat_err);
    if (lat_err != OGRERR_NONE)
        center_lat = OSRGetProjParm(srs, SRS_PP_LATITUDE_OF_CENTER, 0.0, NULL);

    OGRErr lon_err = OGRERR_NONE;
    double center_lon = OSRGetProjParm(srs, SRS_PP_CENTRAL_MERIDIAN, 0.0, &lon_err);
    if (lon_err != OGRERR_NONE)
        center_lon = OSRGetProjParm(srs, SRS_PP_LONGITUDE_OF_CENTER, 0.0, NULL);
    if (lon_err != OGRERR_NONE)
        center_lon = OSRGetProjParm(srs, SRS_PP_LONGITUDE_OF_ORIGIN, center_lon, NULL);

    OGRErr std_err = OGRERR_NONE;
    double standard_parallel = OSRGetProjParm(srs, SRS_PP_PSEUDO_STD_PARALLEL_1, 0.0, &std_err);
    if (std_err != OGRERR_NONE)
        standard_parallel = OSRGetProjParm(srs, SRS_PP_STANDARD_PARALLEL_1, 0.0, NULL);

    out_info->cylindrical.center_latitude = center_lat;
    out_info->cylindrical.center_longitude = center_lon;
    out_info->cylindrical.standard_parallel = standard_parallel;
    out_info->cylindrical.false_easting = OSRGetProjParm(srs, SRS_PP_FALSE_EASTING, 0.0, NULL) * out_info->to_meters;
    out_info->cylindrical.false_northing = OSRGetProjParm(srs, SRS_PP_FALSE_NORTHING, 0.0, NULL) * out_info->to_meters;
    return true;
}

static void _planet_split_double(double value, float *high_out, float *low_out) {
    *high_out = (float)value;
    *low_out = (float)(value - (double)*high_out);
}

static plVec3d _mul_vec3d_scalar(plVec3d v, double s) {
    return (plVec3d){v.x * s, v.y * s, v.z * s};
}

static plVec3d _sub_vec3d(plVec3d a, plVec3d b) {
    return (plVec3d){a.x - b.x, a.y - b.y, a.z - b.z};
}

static plVec3d _cross_vec3d(plVec3d a, plVec3d b) {
    return (plVec3d){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static double _dot_vec3d(plVec3d a, plVec3d b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static plVec3d _normalize_vec3d(plVec3d v) {
    const double len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.0)
        return (plVec3d){0.0, 1.0, 0.0};
    return (plVec3d){v.x / len, v.y / len, v.z / len};
}

static plVec3d _face_uv_to_direction_unclamped(plPlanetFace face, double u, double v) {
    const double s = u * 2.0 - 1.0;
    const double t = v * 2.0 - 1.0;

    switch (face) {
        case PL_PLANET_FACE_POS_X: return _normalize_vec3d((plVec3d){ 1.0, t, -s});
        case PL_PLANET_FACE_NEG_X: return _normalize_vec3d((plVec3d){-1.0, t,  s});
        case PL_PLANET_FACE_POS_Y: return _normalize_vec3d((plVec3d){ s,   1.0, -t});
        case PL_PLANET_FACE_NEG_Y: return _normalize_vec3d((plVec3d){ s,  -1.0,  t});
        case PL_PLANET_FACE_POS_Z: return _normalize_vec3d((plVec3d){ s,   t,   1.0});
        case PL_PLANET_FACE_NEG_Z: return _normalize_vec3d((plVec3d){-s,   t,  -1.0});
        default:                   return (plVec3d){0.0, 0.0, 1.0};
    }
}

static plVec3d _geodetic_to_direction(double latitude, double longitude) {
    const double lat = latitude * M_PI / 180.0;
    const double lon = longitude * M_PI / 180.0;
    const double cos_lat = cos(lat);
    return _normalize_vec3d((plVec3d){
        cos_lat * sin(lon),
        sin(lat),
        cos_lat * cos(lon)
    });
}

static int _compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

//-----------------------------------------------------------------------------
// [SECTION] GDAL helpers
//-----------------------------------------------------------------------------

static double _estimate_projected_mpp(const double gt[6], double to_meters) {
    double sx = hypot(gt[1], gt[4]) * to_meters;
    double sy = hypot(gt[2], gt[5]) * to_meters;
    if (sx <= 0.0)
        return sy;
    if (sy <= 0.0)
        return sx;
    return 0.5 * (sx + sy);
}

static bool _read_dem_metadata(const DcPlanetDemInput *input, double radius_override, DcPlanetDemInfo *out_info) {
    memset(out_info, 0, sizeof(*out_info));
    out_info->band_scale = 1.0;
    out_info->band_offset = 0.0;
    out_info->to_meters = 1.0;

    GDALDatasetH ds = GDALOpen(input->path, GA_ReadOnly);
    if (!ds) {
        fprintf(stderr, "Error: GDALOpen failed for: %s\n", input->path);
        return false;
    }

    out_info->width = (uint32_t)GDALGetRasterXSize(ds);
    out_info->height = (uint32_t)GDALGetRasterYSize(ds);
    if (out_info->width == 0 || out_info->height == 0) {
        fprintf(stderr, "Error: DEM has invalid raster size: %s\n", input->path);
        GDALClose(ds);
        return false;
    }

    OGRSpatialReferenceH srs = GDALGetSpatialRef(ds);
    if (!srs) {
        fprintf(stderr, "Error: DEM is missing spatial reference metadata: %s\n", input->path);
        GDALClose(ds);
        return false;
    }

    OGRErr err = OGRERR_NONE;
    double semi_major = OSRGetSemiMajor(srs, &err);
    if (radius_override > 0.0)
        out_info->radius = radius_override;
    else if (err == OGRERR_NONE && semi_major > 0.0)
        out_info->radius = semi_major;

    if (GDALGetGeoTransform(ds, out_info->gt) != CE_None) {
        fprintf(stderr, "Error: DEM is missing a geotransform: %s\n", input->path);
        GDALClose(ds);
        return false;
    }

    if (OSRIsGeographic(srs)) {
        if (!_read_cylindrical_projection(srs, true, out_info)) {
            GDALClose(ds);
            return false;
        }
    } else {
        const char *projection_name = OSRGetAttrValue(srs, "PROJECTION", 0);
        if (_is_equirectangular_projection(projection_name)) {
            if (!_read_cylindrical_projection(srs, false, out_info)) {
                GDALClose(ds);
                return false;
            }
        } else if (projection_name &&
                   (strcmp(projection_name, SRS_PT_POLAR_STEREOGRAPHIC) == 0 ||
                    strcmp(projection_name, SRS_PT_STEREOGRAPHIC) == 0)) {
            out_info->projection_type = DC_PLANET_DEM_PROJECTION_POLAR_STEREOGRAPHIC;
            out_info->to_meters = OSRGetLinearUnits(srs, NULL);
            if (out_info->to_meters <= 0.0)
                out_info->to_meters = 1.0;

            OGRErr proj_err = OGRERR_NONE;
            double lat_origin = OSRGetProjParm(srs, SRS_PP_LATITUDE_OF_ORIGIN, -90.0, &proj_err);
            OGRErr std_parallel_err = OGRERR_NONE;
            double standard_parallel = OSRGetProjParm(srs, SRS_PP_STANDARD_PARALLEL_1, lat_origin, &std_parallel_err);
            if (proj_err != OGRERR_NONE && std_parallel_err == OGRERR_NONE)
                lat_origin = standard_parallel;
            if (fabs(lat_origin) < 45.0) {
                fprintf(stderr, "Error: polar stereographic source lacks polar latitude of origin: %s\n", input->path);
                GDALClose(ds);
                return false;
            }
            out_info->polar.latitude_of_origin = lat_origin >= 0.0 ? 90.0 : -90.0;

            OGRErr lon_err = OGRERR_NONE;
            double lon_origin = OSRGetProjParm(srs, SRS_PP_CENTRAL_MERIDIAN, 0.0, &lon_err);
            if (lon_err != OGRERR_NONE)
                lon_origin = OSRGetProjParm(srs, SRS_PP_LONGITUDE_OF_ORIGIN, 0.0, NULL);
            out_info->polar.longitude_of_origin = lon_origin;

            OGRErr scale_err = OGRERR_NONE;
            double scale_factor = OSRGetProjParm(srs, SRS_PP_SCALE_FACTOR, 1.0, &scale_err);
            if (scale_err != OGRERR_NONE) {
                double lat_ts = (std_parallel_err == OGRERR_NONE) ? standard_parallel : lat_origin;
                scale_factor = (fabs(fabs(lat_ts) - 90.0) <= 1.0e-9)
                    ? 1.0
                    : 0.5 * (1.0 + sin(fabs(lat_ts) * M_PI / 180.0));
            }
            if (scale_factor <= 0.0)
                scale_factor = 1.0;
            out_info->polar.scale_factor = scale_factor;
            out_info->polar.false_easting = OSRGetProjParm(srs, SRS_PP_FALSE_EASTING, 0.0, NULL) * out_info->to_meters;
            out_info->polar.false_northing = OSRGetProjParm(srs, SRS_PP_FALSE_NORTHING, 0.0, NULL) * out_info->to_meters;
        } else {
            fprintf(stderr, "Error: unsupported DEM projection for planet bootstrap: %s\n", input->path);
            GDALClose(ds);
            return false;
        }
    }

    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    if (!band) {
        fprintf(stderr, "Error: DEM has no raster band: %s\n", input->path);
        GDALClose(ds);
        return false;
    }

    int has_scale = 0;
    double band_scale = GDALGetRasterScale(band, &has_scale);
    if (has_scale && band_scale != 0.0)
        out_info->band_scale = band_scale;
    int has_offset = 0;
    double band_offset = GDALGetRasterOffset(band, &has_offset);
    if (has_offset)
        out_info->band_offset = band_offset;

    double min_raw = 0.0, max_raw = 0.0;
    CPLErr stat_err = GDALComputeRasterStatistics(band, FALSE, &min_raw, &max_raw, NULL, NULL, NULL, NULL);
    if (stat_err != CE_None) {
        fprintf(stderr, "Error: GDALComputeRasterStatistics failed: %s\n", input->path);
        GDALClose(ds);
        return false;
    }

    char stem[128];
    _get_stem(input->path, stem, sizeof(stem));
    strncpy(out_info->source.acName, stem, sizeof(out_info->source.acName) - 1);
    strncpy(out_info->source.acPath, input->path, sizeof(out_info->source.acPath) - 1);
    out_info->source.uPriority = input->priority;
    out_info->source.dMinHeight = out_info->band_offset + min_raw * out_info->band_scale;
    out_info->source.dMaxHeight = out_info->band_offset + max_raw * out_info->band_scale;

    if (out_info->projection_type == DC_PLANET_DEM_PROJECTION_CYLINDRICAL && out_info->cylindrical.angular) {
        double radius = out_info->radius > 0.0 ? out_info->radius : radius_override;
        double deg_m = radius > 0.0 ? radius * M_PI / 180.0 : 1.0;
        double sx = hypot(out_info->gt[1], out_info->gt[4]) * out_info->cylindrical.angular_to_degrees * deg_m;
        double sy = hypot(out_info->gt[2], out_info->gt[5]) * out_info->cylindrical.angular_to_degrees * deg_m;
        out_info->source.dNativeMetersPerPixel = (sx > 0.0 && sy > 0.0) ? 0.5 * (sx + sy) : fmax(sx, sy);
    } else if (out_info->projection_type == DC_PLANET_DEM_PROJECTION_CYLINDRICAL) {
        double sx = hypot(out_info->gt[1], out_info->gt[4]) * out_info->to_meters;
        double sy = hypot(out_info->gt[2], out_info->gt[5]) * out_info->to_meters;
        out_info->source.dNativeMetersPerPixel = (sx > 0.0 && sy > 0.0) ? 0.5 * (sx + sy) : fmax(sx, sy);
    } else {
        out_info->source.dNativeMetersPerPixel = _estimate_projected_mpp(out_info->gt, out_info->to_meters);
    }

    GDALClose(ds);

    if (!_compute_footprint(out_info))
        return false;

    return true;
}

static bool _open_dem_runtimes(const DcPlanetDemInfo *dem_infos, uint32_t dem_count, DcPlanetDemRuntime *runtimes) {
    for (uint32_t i = 0; i < dem_count; i++) {
        const DcPlanetDemInfo *info = &dem_infos[i];
        DcPlanetDemRuntime *runtime = &runtimes[i];

        runtime->dataset = GDALOpen(info->source.acPath, GA_ReadOnly);
        if (!runtime->dataset) {
            fprintf(stderr, "Error: GDALOpen failed for payload source: %s\n", info->source.acPath);
            return false;
        }

        runtime->band = GDALGetRasterBand(runtime->dataset, 1);
        if (!runtime->band) {
            fprintf(stderr, "Error: DEM has no payload raster band: %s\n", info->source.acPath);
            return false;
        }

        int has_nodata = 0;
        runtime->nodata = GDALGetRasterNoDataValue(runtime->band, &has_nodata);
        runtime->has_nodata = has_nodata;

        if (!GDALInvGeoTransform(info->gt, runtime->inv_gt)) {
            fprintf(stderr, "Error: could not invert geotransform: %s\n", info->source.acPath);
            return false;
        }

        if (info->projection_type == DC_PLANET_DEM_PROJECTION_POLAR_STEREOGRAPHIC) {
            OGRSpatialReferenceH source_srs = GDALGetSpatialRef(runtime->dataset);
            if (!source_srs) {
                fprintf(stderr, "Error: DEM is missing SRS during payload setup: %s\n", info->source.acPath);
                return false;
            }

            runtime->source_srs = OSRClone(source_srs);
            runtime->geodetic_srs = OSRCloneGeogCS(source_srs);
            if (!runtime->source_srs || !runtime->geodetic_srs) {
                fprintf(stderr, "Error: failed to clone SRS for source: %s\n", info->source.acPath);
                return false;
            }

            OSRSetAxisMappingStrategy(runtime->source_srs, OAMS_TRADITIONAL_GIS_ORDER);
            OSRSetAxisMappingStrategy(runtime->geodetic_srs, OAMS_TRADITIONAL_GIS_ORDER);

            runtime->geodetic_to_source = OCTNewCoordinateTransformation(runtime->geodetic_srs, runtime->source_srs);
            if (!runtime->geodetic_to_source) {
                fprintf(stderr, "Error: failed to create geodetic->source transform: %s\n", info->source.acPath);
                return false;
            }
        }
    }
    return true;
}

static void _close_dem_runtimes(DcPlanetDemRuntime *runtimes, uint32_t dem_count) {
    if (!runtimes)
        return;
    for (uint32_t i = 0; i < dem_count; i++) {
        if (runtimes[i].geodetic_to_source)
            OCTDestroyCoordinateTransformation(runtimes[i].geodetic_to_source);
        if (runtimes[i].geodetic_srs)
            OSRDestroySpatialReference(runtimes[i].geodetic_srs);
        if (runtimes[i].source_srs)
            OSRDestroySpatialReference(runtimes[i].source_srs);
        if (runtimes[i].dataset)
            GDALClose(runtimes[i].dataset);
    }
}

static bool _choose_tile_primary_source(const plPlanetManifest *manifest, plPlanetFace face, uint8_t lod, uint32_t x, uint32_t y, uint32_t *out_source_index) {
    if (out_source_index)
        *out_source_index = UINT32_MAX;
    if (!manifest || !out_source_index)
        return false;

    const uint32_t dim = 1u << lod;
    bool found = false;
    uint32_t best_index = UINT32_MAX;
    int32_t best_priority = INT32_MIN;
    double best_mpp = DBL_MAX;

    for (uint32_t sample_y = 0; sample_y < DC_PLANET_TILE_SOURCE_SAMPLE_GRID; sample_y++) {
        const double local_v = (double)sample_y / (double)(DC_PLANET_TILE_SOURCE_SAMPLE_GRID - 1u);
        for (uint32_t sample_x = 0; sample_x < DC_PLANET_TILE_SOURCE_SAMPLE_GRID; sample_x++) {
            const double local_u = (double)sample_x / (double)(DC_PLANET_TILE_SOURCE_SAMPLE_GRID - 1u);
            const double u = ((double)x + local_u) / (double)dim;
            const double v = ((double)y + local_v) / (double)dim;
            plVec3d dir = _ext_planet->face_uv_to_direction(face, u, v);
            const double lat = asin(dir.y) * 180.0 / M_PI;
            const double lon = _normalize_lon(atan2(dir.x, dir.z) * 180.0 / M_PI);

            uint32_t candidate_index = UINT32_MAX;
            if (!_ext_planet->choose_source(manifest, lat, lon, 0.0, &candidate_index))
                continue;

            const plPlanetSourceRecord *source = _ext_planet->get_source(manifest, candidate_index);
            if (!source)
                continue;

            const double mpp = source->dNativeMetersPerPixel > 0.0 ? source->dNativeMetersPerPixel : DBL_MAX;
            bool better = !found;
            if (found && source->uPriority > best_priority)
                better = true;
            else if (found && source->uPriority == best_priority && mpp < best_mpp)
                better = true;

            if (better) {
                found = true;
                best_index = candidate_index;
                best_priority = source->uPriority;
                best_mpp = mpp;
            }
        }
    }

    if (!found)
        return false;
    *out_source_index = best_index;
    return true;
}

static bool _source_record_contains(const plPlanetSourceRecord *source, double latitude, double longitude) {
    if (!source)
        return false;
    if (latitude < source->dMinLatitude || latitude > source->dMaxLatitude)
        return false;

    if ((source->tFlags & PL_PLANET_SOURCE_FLAGS_GLOBAL_LONGITUDE) != 0)
        return true;

    const double lon = _normalize_lon(longitude);
    const double min_lon = _normalize_lon(source->dMinLongitude);
    const double max_lon = _normalize_lon(source->dMaxLongitude);
    if (min_lon <= max_lon)
        return lon >= min_lon && lon <= max_lon;
    return lon >= min_lon || lon <= max_lon;
}

static bool _source_latlon_to_pixel(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, double latitude, double longitude, double *out_pixel_x, double *out_pixel_y) {
    if (!info || !runtime || !out_pixel_x || !out_pixel_y)
        return false;

    if (info->projection_type == DC_PLANET_DEM_PROJECTION_CYLINDRICAL)
        return _cylindrical_latlon_to_pixel(info, runtime, latitude, longitude, out_pixel_x, out_pixel_y);

    if (!runtime->geodetic_to_source)
        return false;

    double lon_norm = _normalize_lon(longitude);
    double lon_pm = lon_norm > 180.0 ? lon_norm - 360.0 : lon_norm;
    double lon_candidates[3] = {
        lon_pm,
        lon_norm,
        lon_pm < 0.0 ? lon_pm + 360.0 : lon_pm - 360.0
    };

    for (uint32_t i = 0; i < 3; i++) {
        double x = lon_candidates[i];
        double y = latitude;
        double z = 0.0;
        if (!OCTTransform(runtime->geodetic_to_source, 1, &x, &y, &z))
            continue;

        double px = runtime->inv_gt[0] + runtime->inv_gt[1] * x + runtime->inv_gt[2] * y;
        double py = runtime->inv_gt[3] + runtime->inv_gt[4] * x + runtime->inv_gt[5] * y;
        if (_accept_pixel_for_sampling(info, px, py, out_pixel_x, out_pixel_y))
            return true;
    }

    return false;
}

static bool _cylindrical_latlon_to_pixel(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, double latitude, double longitude, double *out_pixel_x, double *out_pixel_y) {
    double lon_norm = _normalize_lon(longitude);
    double lon_pm = lon_norm > 180.0 ? lon_norm - 360.0 : lon_norm;
    double lon_candidates[4] = {
        lon_pm,
        lon_norm,
        lon_pm + 360.0,
        lon_pm - 360.0
    };

    const double radius = info->radius;
    const double cos_standard = cos(info->cylindrical.standard_parallel * M_PI / 180.0);
    if (!info->cylindrical.angular && (radius <= 0.0 || fabs(cos_standard) <= 1.0e-12 || info->to_meters <= 0.0))
        return false;

    for (uint32_t i = 0; i < 4; i++) {
        double x = 0.0;
        double y = 0.0;
        if (info->cylindrical.angular) {
            x = lon_candidates[i] / info->cylindrical.angular_to_degrees;
            y = latitude / info->cylindrical.angular_to_degrees;
        } else {
            const double dlon = (lon_candidates[i] - info->cylindrical.center_longitude) * M_PI / 180.0;
            const double dlat = (latitude - info->cylindrical.center_latitude) * M_PI / 180.0;
            x = (info->cylindrical.false_easting + radius * dlon * cos_standard) / info->to_meters;
            y = (info->cylindrical.false_northing + radius * dlat) / info->to_meters;
        }

        double px = runtime->inv_gt[0] + runtime->inv_gt[1] * x + runtime->inv_gt[2] * y;
        double py = runtime->inv_gt[3] + runtime->inv_gt[4] * x + runtime->inv_gt[5] * y;
        if (_accept_pixel_for_sampling(info, px, py, out_pixel_x, out_pixel_y))
            return true;
    }

    return false;
}

static bool _accept_pixel_for_sampling(const DcPlanetDemInfo *info, double pixel_x, double pixel_y, double *out_pixel_x, double *out_pixel_y) {
    if (!info || info->width == 0 || info->height == 0 || !out_pixel_x || !out_pixel_y)
        return false;

    // GDAL geotransforms map the outer raster edge to width/height. Samples on
    // that edge are valid and are clamped to the last texel by the sampler.
    const double eps = 1.0e-7;
    const double max_x = (double)info->width;
    const double max_y = (double)info->height;
    if (pixel_x < -eps || pixel_y < -eps || pixel_x > max_x + eps || pixel_y > max_y + eps)
        return false;

    *out_pixel_x = pixel_x < 0.0 ? 0.0 : (pixel_x > max_x ? max_x : pixel_x);
    *out_pixel_y = pixel_y < 0.0 ? 0.0 : (pixel_y > max_y ? max_y : pixel_y);
    return true;
}

static bool _choose_sample_source(const DcPlanetDemInfo *dem_infos, const DcPlanetDemRuntime *runtimes, uint32_t dem_count, double latitude, double longitude, uint32_t *out_source_index, double *out_pixel_x, double *out_pixel_y) {
    bool found = false;
    uint32_t best_index = UINT32_MAX;
    int32_t best_priority = INT32_MIN;
    double best_mpp = DBL_MAX;
    double best_px = 0.0;
    double best_py = 0.0;

    for (uint32_t i = 0; i < dem_count; i++) {
        const plPlanetSourceRecord *source = &dem_infos[i].source;
        if (!_source_record_contains(source, latitude, longitude))
            continue;

        double px = 0.0;
        double py = 0.0;
        if (!_source_latlon_to_pixel(&dem_infos[i], &runtimes[i], latitude, longitude, &px, &py))
            continue;

        const double mpp = source->dNativeMetersPerPixel > 0.0 ? source->dNativeMetersPerPixel : DBL_MAX;
        bool better = !found;
        if (found && source->uPriority > best_priority)
            better = true;
        else if (found && source->uPriority == best_priority && mpp < best_mpp)
            better = true;

        if (better) {
            found = true;
            best_index = i;
            best_priority = source->uPriority;
            best_mpp = mpp;
            best_px = px;
            best_py = py;
        }
    }

    if (!found)
        return false;

    *out_source_index = best_index;
    *out_pixel_x = best_px;
    *out_pixel_y = best_py;
    return true;
}

static bool _pixel_to_geodetic(const DcPlanetDemInfo *info, double pixel_x, double pixel_y, double *out_lat, double *out_lon) {
    double a = info->gt[0] + pixel_x * info->gt[1] + pixel_y * info->gt[2];
    double b = info->gt[3] + pixel_x * info->gt[4] + pixel_y * info->gt[5];

    if (info->projection_type == DC_PLANET_DEM_PROJECTION_CYLINDRICAL) {
        if (info->cylindrical.angular) {
            *out_lon = _normalize_lon(a * info->cylindrical.angular_to_degrees);
            *out_lat = b * info->cylindrical.angular_to_degrees;
            return true;
        }

        double radius = info->radius;
        double cos_standard = cos(info->cylindrical.standard_parallel * M_PI / 180.0);
        if (radius <= 0.0 || fabs(cos_standard) <= 1.0e-12)
            return false;

        double x = a * info->to_meters - info->cylindrical.false_easting;
        double y = b * info->to_meters - info->cylindrical.false_northing;
        double lat = info->cylindrical.center_latitude + (y / radius) * 180.0 / M_PI;
        double lon = info->cylindrical.center_longitude + (x / (radius * cos_standard)) * 180.0 / M_PI;
        *out_lon = _normalize_lon(lon);
        *out_lat = lat;
        return true;
    }

    double x = a * info->to_meters - info->polar.false_easting;
    double y = b * info->to_meters - info->polar.false_northing;
    double radius = info->radius;
    if (radius <= 0.0)
        return false;

    double rho = hypot(x, y);
    double c = 2.0 * atan(rho / (2.0 * radius * info->polar.scale_factor));
    bool north = info->polar.latitude_of_origin > 0.0;
    double phi = rho == 0.0
        ? (north ? M_PI_2 : -M_PI_2)
        : (north ? M_PI_2 - c : -M_PI_2 + c);
    double lam0 = info->polar.longitude_of_origin * M_PI / 180.0;
    double lam = lam0 + (north ? atan2(x, -y) : atan2(x, y));

    *out_lat = phi * 180.0 / M_PI;
    *out_lon = _normalize_lon(lam * 180.0 / M_PI);
    return true;
}

static bool _compute_footprint(DcPlanetDemInfo *info) {
    double min_lat = DBL_MAX;
    double max_lat = -DBL_MAX;
    double lons[DC_PLANET_FOOTPRINT_SAMPLES * 4u + 8u] = {0};
    uint32_t lon_count = 0;

    const double max_x = (double)info->width;
    const double max_y = (double)info->height;
    for (uint32_t i = 0; i < DC_PLANET_FOOTPRINT_SAMPLES; i++) {
        double t = (double)i / (double)(DC_PLANET_FOOTPRINT_SAMPLES - 1u);
        double samples[4][2] = {
            {t * max_x, 0.0},
            {t * max_x, max_y},
            {0.0, t * max_y},
            {max_x, t * max_y},
        };
        for (uint32_t s = 0; s < 4; s++) {
            if (!_accumulate_footprint_sample(info, samples[s][0], samples[s][1], &min_lat, &max_lat, lons, &lon_count))
                return false;
        }
    }

    // Polar DEMs often contain the pole inside the image, not on the perimeter.
    // Include the image center and exact projection origin when available so the
    // footprint describes a cap instead of a ring.
    if (!_accumulate_footprint_sample(info, max_x * 0.5, max_y * 0.5, &min_lat, &max_lat, lons, &lon_count))
        return false;

    if (info->projection_type == DC_PLANET_DEM_PROJECTION_POLAR_STEREOGRAPHIC) {
        const double det = info->gt[1] * info->gt[5] - info->gt[2] * info->gt[4];
        if (fabs(det) > 1.0e-18 && info->to_meters != 0.0) {
            const double origin_x = info->polar.false_easting / info->to_meters;
            const double origin_y = info->polar.false_northing / info->to_meters;
            const double dx = origin_x - info->gt[0];
            const double dy = origin_y - info->gt[3];
            const double pixel_x = ( info->gt[5] * dx - info->gt[2] * dy) / det;
            const double pixel_y = (-info->gt[4] * dx + info->gt[1] * dy) / det;
            if (pixel_x >= 0.0 && pixel_x <= max_x && pixel_y >= 0.0 && pixel_y <= max_y) {
                if (!_accumulate_footprint_sample(info, pixel_x, pixel_y, &min_lat, &max_lat, lons, &lon_count))
                    return false;
            }
        }
    }

    qsort(lons, lon_count, sizeof(double), _compare_double);

    double largest_gap = -1.0;
    uint32_t gap_index = 0;
    for (uint32_t i = 0; i < lon_count; i++) {
        uint32_t next = (i + 1u) % lon_count;
        double current = lons[i];
        double next_lon = lons[next] + (next == 0 ? 360.0 : 0.0);
        double gap = next_lon - current;
        if (gap > largest_gap) {
            largest_gap = gap;
            gap_index = i;
        }
    }

    const double global_gap_threshold = (360.0 / (double)(DC_PLANET_FOOTPRINT_SAMPLES - 1u)) * 1.25;
    bool global_longitude = largest_gap <= global_gap_threshold;

    uint32_t start_index = (gap_index + 1u) % lon_count;
    double min_lon = global_longitude ? 0.0 : lons[start_index];
    double max_lon = global_longitude ? 0.0 : lons[gap_index];

    info->source.dMinLatitude = min_lat;
    info->source.dMaxLatitude = max_lat;
    info->source.dMinLongitude = min_lon;
    info->source.dMaxLongitude = max_lon;
    if (global_longitude)
        info->source.tFlags |= PL_PLANET_SOURCE_FLAGS_GLOBAL_LONGITUDE;
    return true;
}

static bool _accumulate_footprint_sample(const DcPlanetDemInfo *info, double pixel_x, double pixel_y, double *min_lat, double *max_lat, double *lons, uint32_t *lon_count) {
    double lat = 0.0;
    double lon = 0.0;
    if (!_pixel_to_geodetic(info, pixel_x, pixel_y, &lat, &lon))
        return false;
    if (lat < *min_lat) *min_lat = lat;
    if (lat > *max_lat) *max_lat = lat;
    lons[(*lon_count)++] = _normalize_lon(lon);
    return true;
}

static bool _read_source_window(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, int xoff, int yoff, int width, int height, DcPlanetSourceWindow *out_window) {
    (void)info;
    memset(out_window, 0, sizeof(*out_window));
    if (width <= 0 || height <= 0)
        return false;

    size_t pixel_count = (size_t)width * (size_t)height;
    float *pixels = (float *)malloc(pixel_count * sizeof(float));
    if (!pixels)
        return false;

    CPLErr err = GDALRasterIO(runtime->band, GF_Read, xoff, yoff, width, height, pixels, width, height, GDT_Float32, 0, 0);
    if (err != CE_None) {
        free(pixels);
        return false;
    }

    out_window->xoff = xoff;
    out_window->yoff = yoff;
    out_window->width = width;
    out_window->height = height;
    out_window->pixels = pixels;
    return true;
}

static bool _sample_source_height(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, const DcPlanetSourceWindow *window, double pixel_x, double pixel_y, double *out_height) {
    int x0 = (int)floor(pixel_x);
    int y0 = (int)floor(pixel_y);
    double tx = pixel_x - (double)x0;
    double ty = pixel_y - (double)y0;

    if (info->width == 0 || info->height == 0)
        return false;

    if (info->width == 1) {
        x0 = 0;
        tx = 0.0;
    } else if (x0 < 0) {
        x0 = 0;
        tx = 0.0;
    } else if (x0 >= (int)info->width - 1) {
        x0 = (int)info->width - 2;
        tx = 1.0;
    }

    if (info->height == 1) {
        y0 = 0;
        ty = 0.0;
    } else if (y0 < 0) {
        y0 = 0;
        ty = 0.0;
    } else if (y0 >= (int)info->height - 1) {
        y0 = (int)info->height - 2;
        ty = 1.0;
    }

    float raw[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool have_window = false;
    if (window && window->pixels) {
        int local_x = x0 - window->xoff;
        int local_y = y0 - window->yoff;
        if (local_x >= 0 && local_y >= 0 && local_x + 1 < window->width && local_y + 1 < window->height) {
            raw[0] = window->pixels[local_x + local_y * window->width];
            raw[1] = window->pixels[(local_x + 1) + local_y * window->width];
            raw[2] = window->pixels[local_x + (local_y + 1) * window->width];
            raw[3] = window->pixels[(local_x + 1) + (local_y + 1) * window->width];
            have_window = true;
        }
    }

    if (!have_window) {
        CPLErr err = GDALRasterIO(runtime->band, GF_Read, x0, y0, 2, 2, raw, 2, 2, GDT_Float32, 0, 0);
        if (err != CE_None)
            return false;
    }

    if (runtime->has_nodata) {
        for (uint32_t i = 0; i < 4; i++) {
            if (fabs((double)raw[i] - runtime->nodata) <= 1.0e-6)
                return false;
        }
    }

    double h00 = info->band_offset + (double)raw[0] * info->band_scale;
    double h10 = info->band_offset + (double)raw[1] * info->band_scale;
    double h01 = info->band_offset + (double)raw[2] * info->band_scale;
    double h11 = info->band_offset + (double)raw[3] * info->band_scale;
    double h0 = h00 * (1.0 - tx) + h10 * tx;
    double h1 = h01 * (1.0 - tx) + h11 * tx;
    *out_height = h0 * (1.0 - ty) + h1 * ty;
    return true;
}

static bool _source_height_is_radial(const DcPlanetDemInfo *info, double radius, DcPlanetHeightMode height_mode) {
    if (height_mode == DC_PLANET_HEIGHT_MODE_RADIAL)
        return true;
    if (height_mode == DC_PLANET_HEIGHT_MODE_OFFSET)
        return false;
    const double source_mid = 0.5 * (info->source.dMinHeight + info->source.dMaxHeight);
    return source_mid > radius * 0.5 && source_mid < radius * 1.5;
}

static double _height_to_radial_distance(const DcPlanetDemInfo *info, double radius, double height, DcPlanetHeightMode height_mode) {
    return radius + _height_to_elevation_offset(info, radius, height, height_mode);
}

static double _height_to_elevation_offset(const DcPlanetDemInfo *info, double radius, double height, DcPlanetHeightMode height_mode) {
    return _source_height_is_radial(info, radius, height_mode) ? height - radius : height;
}

static plVec3d _sample_planet_position_at_face_uv(plPlanetFace face, double face_u, double face_v, const DcPlanetDemInfo *dem_infos, const DcPlanetDemRuntime *runtimes, const DcPlanetSourceWindow *windows, uint32_t dem_count, double radius, DcPlanetHeightMode height_mode) {
    plVec3d dir = _face_uv_to_direction_unclamped(face, face_u, face_v);
    const double lat = asin(dir.y) * 180.0 / M_PI;
    const double lon = _normalize_lon(atan2(dir.x, dir.z) * 180.0 / M_PI);

    uint32_t source_index = UINT32_MAX;
    double px = 0.0;
    double py = 0.0;
    if (_choose_sample_source(dem_infos, runtimes, dem_count, lat, lon, &source_index, &px, &py)) {
        double height = 0.0;
        const DcPlanetSourceWindow *window = windows ? &windows[source_index] : NULL;
        if (_sample_source_height(&dem_infos[source_index], &runtimes[source_index], window, px, py, &height))
            return _mul_vec3d_scalar(dir, _height_to_radial_distance(&dem_infos[source_index], radius, height, height_mode));
    }

    return _mul_vec3d_scalar(dir, radius);
}

static bool _sample_planet_position_at_source_pixel(const DcPlanetDemInfo *info, const DcPlanetDemRuntime *runtime, const DcPlanetSourceWindow *window, double pixel_x, double pixel_y, double radius, DcPlanetHeightMode height_mode, plVec3d *out_position) {
    if (!info || !runtime || !out_position || info->width == 0 || info->height == 0)
        return false;

    double clamped_x = pixel_x;
    double clamped_y = pixel_y;
    if (clamped_x < 0.0) clamped_x = 0.0;
    if (clamped_y < 0.0) clamped_y = 0.0;
    if (clamped_x > (double)info->width) clamped_x = (double)info->width;
    if (clamped_y > (double)info->height) clamped_y = (double)info->height;

    double lat = 0.0;
    double lon = 0.0;
    if (!_pixel_to_geodetic(info, clamped_x, clamped_y, &lat, &lon))
        return false;

    double height = 0.0;
    if (!_sample_source_height(info, runtime, window, clamped_x, clamped_y, &height))
        return false;

    plVec3d dir = _geodetic_to_direction(lat, lon);
    *out_position = _mul_vec3d_scalar(dir, _height_to_radial_distance(info, radius, height, height_mode));
    return true;
}

static void _write_skirt_vertex(plPlanetVertex *vertices, uint32_t dst_index, const plPlanetVertex *src_vertex, plVec3d src_position, double skirt_depth, plVec3d *min_bound, plVec3d *max_bound) {
    plVec3d radial = _normalize_vec3d(src_position);
    plVec3d skirt_position = _sub_vec3d(src_position, _mul_vec3d_scalar(radial, skirt_depth));

    vertices[dst_index] = *src_vertex;
    _planet_split_double(skirt_position.x, &vertices[dst_index].tPositionHigh.x, &vertices[dst_index].tPositionLow.x);
    _planet_split_double(skirt_position.y, &vertices[dst_index].tPositionHigh.y, &vertices[dst_index].tPositionLow.y);
    _planet_split_double(skirt_position.z, &vertices[dst_index].tPositionHigh.z, &vertices[dst_index].tPositionLow.z);
    vertices[dst_index].fHeight = src_vertex->fHeight - (float)skirt_depth;

    if (skirt_position.x < min_bound->x) min_bound->x = skirt_position.x;
    if (skirt_position.y < min_bound->y) min_bound->y = skirt_position.y;
    if (skirt_position.z < min_bound->z) min_bound->z = skirt_position.z;
    if (skirt_position.x > max_bound->x) max_bound->x = skirt_position.x;
    if (skirt_position.y > max_bound->y) max_bound->y = skirt_position.y;
    if (skirt_position.z > max_bound->z) max_bound->z = skirt_position.z;
}

static void _add_skirt_quad_indices(uint32_t *indices, uint32_t *cursor, uint32_t edge0, uint32_t edge1, uint32_t skirt0, uint32_t skirt1) {
    indices[(*cursor)++] = edge0;
    indices[(*cursor)++] = edge1;
    indices[(*cursor)++] = skirt0;
    indices[(*cursor)++] = edge1;
    indices[(*cursor)++] = skirt1;
    indices[(*cursor)++] = skirt0;

    indices[(*cursor)++] = edge0;
    indices[(*cursor)++] = skirt0;
    indices[(*cursor)++] = edge1;
    indices[(*cursor)++] = edge1;
    indices[(*cursor)++] = skirt0;
    indices[(*cursor)++] = skirt1;
}

static bool _write_planet_chunk(const char *output_dir, plPlanetTileRecord *tile, const DcPlanetDemInfo *dem_infos, const DcPlanetDemRuntime *runtimes, uint32_t dem_count, double radius, uint32_t tile_size, DcPlanetHeightMode height_mode, uint32_t max_window_mb) {
    const uint32_t base_vertex_count = tile_size * tile_size;
    const uint32_t cell_count = (tile_size - 1u) * (tile_size - 1u);
    const uint32_t base_index_count = cell_count * 6u;
    const uint32_t skirt_vertex_count = tile_size * 4u;
    const uint32_t skirt_index_count = (tile_size - 1u) * 4u * 12u;
    const uint32_t vertex_count = base_vertex_count + skirt_vertex_count;
    const uint32_t index_count = base_index_count + skirt_index_count;

    plPlanetVertex *vertices = (plPlanetVertex *)calloc(vertex_count, sizeof(plPlanetVertex));
    plVec3d *positions = (plVec3d *)malloc((size_t)vertex_count * sizeof(plVec3d));
    uint32_t *indices = (uint32_t *)malloc((size_t)index_count * sizeof(uint32_t));
    uint32_t *sample_sources = (uint32_t *)malloc((size_t)vertex_count * sizeof(uint32_t));
    double *sample_px = (double *)malloc((size_t)vertex_count * sizeof(double));
    double *sample_py = (double *)malloc((size_t)vertex_count * sizeof(double));
    int *min_x = (int *)malloc((size_t)dem_count * sizeof(int));
    int *min_y = (int *)malloc((size_t)dem_count * sizeof(int));
    int *max_x = (int *)malloc((size_t)dem_count * sizeof(int));
    int *max_y = (int *)malloc((size_t)dem_count * sizeof(int));
    bool *used = (bool *)calloc(dem_count, sizeof(bool));
    DcPlanetSourceWindow *windows = (DcPlanetSourceWindow *)calloc(dem_count, sizeof(DcPlanetSourceWindow));

    if (!vertices || !positions || !indices || !sample_sources || !sample_px || !sample_py || !min_x || !min_y || !max_x || !max_y || !used || !windows) {
        fprintf(stderr, "Error: failed to allocate chunk generation buffers\n");
        free(vertices); free(positions); free(indices); free(sample_sources); free(sample_px); free(sample_py);
        free(min_x); free(min_y); free(max_x); free(max_y); free(used); free(windows);
        return false;
    }

    for (uint32_t i = 0; i < dem_count; i++) {
        min_x[i] = INT_MAX;
        min_y[i] = INT_MAX;
        max_x[i] = INT_MIN;
        max_y[i] = INT_MIN;
    }

    const uint32_t dim = 1u << tile->tAddress.uLod;
    const double denom = (double)(tile_size - 1u);
    for (uint32_t y = 0; y < tile_size; y++) {
        for (uint32_t x = 0; x < tile_size; x++) {
            const uint32_t sample_index = x + y * tile_size;
            const double face_u = ((double)tile->tAddress.uX + (double)x / denom) / (double)dim;
            const double face_v = ((double)tile->tAddress.uY + (double)y / denom) / (double)dim;
            plVec3d dir = _ext_planet->face_uv_to_direction(tile->tAddress.tFace, face_u, face_v);
            const double lat = asin(dir.y) * 180.0 / M_PI;
            const double lon = _normalize_lon(atan2(dir.x, dir.z) * 180.0 / M_PI);

            uint32_t source_index = UINT32_MAX;
            double px = 0.0;
            double py = 0.0;
            if (_choose_sample_source(dem_infos, runtimes, dem_count, lat, lon, &source_index, &px, &py)) {
                sample_sources[sample_index] = source_index;
                sample_px[sample_index] = px;
                sample_py[sample_index] = py;
                used[source_index] = true;

                int sx0 = (int)floor(px) - 1;
                int sy0 = (int)floor(py) - 1;
                int sx1 = (int)ceil(px) + 2;
                int sy1 = (int)ceil(py) + 2;
                if (sx0 < min_x[source_index]) min_x[source_index] = sx0;
                if (sy0 < min_y[source_index]) min_y[source_index] = sy0;
                if (sx1 > max_x[source_index]) max_x[source_index] = sx1;
                if (sy1 > max_y[source_index]) max_y[source_index] = sy1;
            } else {
                sample_sources[sample_index] = UINT32_MAX;
                sample_px[sample_index] = 0.0;
                sample_py[sample_index] = 0.0;
            }
        }
    }

    const uint64_t max_window_bytes = (uint64_t)max_window_mb * 1024ull * 1024ull;
    for (uint32_t i = 0; i < dem_count; i++) {
        if (!used[i])
            continue;

        int x0 = min_x[i] < 0 ? 0 : min_x[i];
        int y0 = min_y[i] < 0 ? 0 : min_y[i];
        int x1 = max_x[i] >= (int)dem_infos[i].width ? (int)dem_infos[i].width - 1 : max_x[i];
        int y1 = max_y[i] >= (int)dem_infos[i].height ? (int)dem_infos[i].height - 1 : max_y[i];
        int width = x1 - x0 + 1;
        int height = y1 - y0 + 1;
        uint64_t bytes = (uint64_t)width * (uint64_t)height * sizeof(float);
        if (width >= 2 && height >= 2 && bytes <= max_window_bytes) {
            if (!_read_source_window(&dem_infos[i], &runtimes[i], x0, y0, width, height, &windows[i])) {
                fprintf(stderr, "Warning: falling back to direct GDAL sampling for source %s\n", dem_infos[i].source.acName);
            }
        }
    }

    plVec3d min_bound = { DBL_MAX, DBL_MAX, DBL_MAX };
    plVec3d max_bound = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
    plVec3d min_bound_flat = { DBL_MAX, DBL_MAX, DBL_MAX };
    plVec3d max_bound_flat = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
    double min_elevation = DBL_MAX;
    double max_elevation = -DBL_MAX;

    for (uint32_t y = 0; y < tile_size; y++) {
        for (uint32_t x = 0; x < tile_size; x++) {
            const uint32_t sample_index = x + y * tile_size;
            const double face_u = ((double)tile->tAddress.uX + (double)x / denom) / (double)dim;
            const double face_v = ((double)tile->tAddress.uY + (double)y / denom) / (double)dim;
            plVec3d dir = _ext_planet->face_uv_to_direction(tile->tAddress.tFace, face_u, face_v);

            double height = 0.0;
            double elevation_offset = 0.0;
            double radial_distance = radius;
            uint32_t source_index = sample_sources[sample_index];
            if (source_index != UINT32_MAX) {
                if (_sample_source_height(&dem_infos[source_index], &runtimes[source_index], &windows[source_index], sample_px[sample_index], sample_py[sample_index], &height)) {
                    elevation_offset = _height_to_elevation_offset(&dem_infos[source_index], radius, height, height_mode);
                    radial_distance = radius + elevation_offset;
                } else {
                    height = 0.0;
                    elevation_offset = 0.0;
                    radial_distance = radius;
                }
            } else {
                height = 0.0;
                elevation_offset = 0.0;
                radial_distance = radius;
            }

            if (elevation_offset < min_elevation) min_elevation = elevation_offset;
            if (elevation_offset > max_elevation) max_elevation = elevation_offset;

            plVec3d pos = _mul_vec3d_scalar(dir, radial_distance);
            plVec3d flat = _mul_vec3d_scalar(dir, radius);
            positions[sample_index] = pos;

            _planet_split_double(pos.x, &vertices[sample_index].tPositionHigh.x, &vertices[sample_index].tPositionLow.x);
            _planet_split_double(pos.y, &vertices[sample_index].tPositionHigh.y, &vertices[sample_index].tPositionLow.y);
            _planet_split_double(pos.z, &vertices[sample_index].tPositionHigh.z, &vertices[sample_index].tPositionLow.z);
            vertices[sample_index].tUV = (plVec2){(float)((double)x / denom), (float)((double)y / denom)};
            vertices[sample_index].fHeight = (float)elevation_offset;

            if (pos.x < min_bound.x) min_bound.x = pos.x;
            if (pos.y < min_bound.y) min_bound.y = pos.y;
            if (pos.z < min_bound.z) min_bound.z = pos.z;
            if (pos.x > max_bound.x) max_bound.x = pos.x;
            if (pos.y > max_bound.y) max_bound.y = pos.y;
            if (pos.z > max_bound.z) max_bound.z = pos.z;
            if (flat.x < min_bound_flat.x) min_bound_flat.x = flat.x;
            if (flat.y < min_bound_flat.y) min_bound_flat.y = flat.y;
            if (flat.z < min_bound_flat.z) min_bound_flat.z = flat.z;
            if (flat.x > max_bound_flat.x) max_bound_flat.x = flat.x;
            if (flat.y > max_bound_flat.y) max_bound_flat.y = flat.y;
            if (flat.z > max_bound_flat.z) max_bound_flat.z = flat.z;
        }
    }

    for (uint32_t y = 0; y < tile_size; y++) {
        for (uint32_t x = 0; x < tile_size; x++) {
            const uint32_t sample_index = x + y * tile_size;
            plVec3d tangent_u = {0.0, 0.0, 0.0};
            plVec3d tangent_v = {0.0, 0.0, 0.0};
            bool have_source_normal = false;

            const uint32_t source_index = sample_sources[sample_index];
            if (source_index != UINT32_MAX) {
                plVec3d left_pos = {0.0, 0.0, 0.0};
                plVec3d right_pos = {0.0, 0.0, 0.0};
                plVec3d up_pos = {0.0, 0.0, 0.0};
                plVec3d down_pos = {0.0, 0.0, 0.0};
                const DcPlanetSourceWindow *window = &windows[source_index];
                const double px = sample_px[sample_index];
                const double py = sample_py[sample_index];

                if (_sample_planet_position_at_source_pixel(&dem_infos[source_index], &runtimes[source_index], window, px - 1.0, py, radius, height_mode, &left_pos) &&
                    _sample_planet_position_at_source_pixel(&dem_infos[source_index], &runtimes[source_index], window, px + 1.0, py, radius, height_mode, &right_pos) &&
                    _sample_planet_position_at_source_pixel(&dem_infos[source_index], &runtimes[source_index], window, px, py - 1.0, radius, height_mode, &up_pos) &&
                    _sample_planet_position_at_source_pixel(&dem_infos[source_index], &runtimes[source_index], window, px, py + 1.0, radius, height_mode, &down_pos)) {
                    tangent_u = _sub_vec3d(right_pos, left_pos);
                    tangent_v = _sub_vec3d(down_pos, up_pos);
                    have_source_normal = _dot_vec3d(_cross_vec3d(tangent_v, tangent_u), _cross_vec3d(tangent_v, tangent_u)) > 1.0e-18;
                }
            }

            if (!have_source_normal) {
                const double face_u = ((double)tile->tAddress.uX + (double)x / denom) / (double)dim;
                const double face_v = ((double)tile->tAddress.uY + (double)y / denom) / (double)dim;
                const double step = 1.0 / ((double)dim * denom);

                const plVec3d left_pos = x > 0u
                    ? positions[(x - 1u) + y * tile_size]
                    : _sample_planet_position_at_face_uv(tile->tAddress.tFace, face_u - step, face_v, dem_infos, runtimes, windows, dem_count, radius, height_mode);
                const plVec3d right_pos = x + 1u < tile_size
                    ? positions[(x + 1u) + y * tile_size]
                    : _sample_planet_position_at_face_uv(tile->tAddress.tFace, face_u + step, face_v, dem_infos, runtimes, windows, dem_count, radius, height_mode);
                const plVec3d up_pos = y > 0u
                    ? positions[x + (y - 1u) * tile_size]
                    : _sample_planet_position_at_face_uv(tile->tAddress.tFace, face_u, face_v - step, dem_infos, runtimes, windows, dem_count, radius, height_mode);
                const plVec3d down_pos = y + 1u < tile_size
                    ? positions[x + (y + 1u) * tile_size]
                    : _sample_planet_position_at_face_uv(tile->tAddress.tFace, face_u, face_v + step, dem_infos, runtimes, windows, dem_count, radius, height_mode);

                tangent_u = _sub_vec3d(right_pos, left_pos);
                tangent_v = _sub_vec3d(down_pos, up_pos);
            }

            plVec3d normal = _normalize_vec3d(_cross_vec3d(tangent_v, tangent_u));
            plVec3d radial = _normalize_vec3d(positions[sample_index]);
            if (_dot_vec3d(normal, radial) < 0.0)
                normal = _mul_vec3d_scalar(normal, -1.0);
            vertices[sample_index].tNormal = (plVec3){(float)normal.x, (float)normal.y, (float)normal.z};
        }
    }

    const double skirt_depth = tile->dGeometricError > 0.0 ? fmax(tile->dGeometricError * 2.0, 100.0) : 100.0;
    const uint32_t north_skirt_start = base_vertex_count;
    const uint32_t south_skirt_start = north_skirt_start + tile_size;
    const uint32_t west_skirt_start = south_skirt_start + tile_size;
    const uint32_t east_skirt_start = west_skirt_start + tile_size;

    for (uint32_t x = 0; x < tile_size; x++) {
        const uint32_t north_edge = x;
        const uint32_t south_edge = x + (tile_size - 1u) * tile_size;
        _write_skirt_vertex(vertices, north_skirt_start + x, &vertices[north_edge], positions[north_edge], skirt_depth, &min_bound, &max_bound);
        _write_skirt_vertex(vertices, south_skirt_start + x, &vertices[south_edge], positions[south_edge], skirt_depth, &min_bound, &max_bound);
    }

    for (uint32_t y = 0; y < tile_size; y++) {
        const uint32_t west_edge = y * tile_size;
        const uint32_t east_edge = (tile_size - 1u) + y * tile_size;
        _write_skirt_vertex(vertices, west_skirt_start + y, &vertices[west_edge], positions[west_edge], skirt_depth, &min_bound, &max_bound);
        _write_skirt_vertex(vertices, east_skirt_start + y, &vertices[east_edge], positions[east_edge], skirt_depth, &min_bound, &max_bound);
    }

    uint32_t index_cursor = 0;
    for (uint32_t y = 0; y < tile_size - 1u; y++) {
        for (uint32_t x = 0; x < tile_size - 1u; x++) {
            const uint32_t i0 = x + y * tile_size;
            const uint32_t i1 = (x + 1u) + y * tile_size;
            const uint32_t i2 = x + (y + 1u) * tile_size;
            const uint32_t i3 = (x + 1u) + (y + 1u) * tile_size;
            indices[index_cursor++] = i0;
            indices[index_cursor++] = i1;
            indices[index_cursor++] = i2;
            indices[index_cursor++] = i1;
            indices[index_cursor++] = i3;
            indices[index_cursor++] = i2;
        }
    }

    for (uint32_t x = 0; x < tile_size - 1u; x++) {
        _add_skirt_quad_indices(indices, &index_cursor,
            x,
            x + 1u,
            north_skirt_start + x,
            north_skirt_start + x + 1u);
        _add_skirt_quad_indices(indices, &index_cursor,
            x + (tile_size - 1u) * tile_size,
            (x + 1u) + (tile_size - 1u) * tile_size,
            south_skirt_start + x,
            south_skirt_start + x + 1u);
    }

    for (uint32_t y = 0; y < tile_size - 1u; y++) {
        _add_skirt_quad_indices(indices, &index_cursor,
            y * tile_size,
            (y + 1u) * tile_size,
            west_skirt_start + y,
            west_skirt_start + y + 1u);
        _add_skirt_quad_indices(indices, &index_cursor,
            (tile_size - 1u) + y * tile_size,
            (tile_size - 1u) + (y + 1u) * tile_size,
            east_skirt_start + y,
            east_skirt_start + y + 1u);
    }

    if (index_cursor != index_count) {
        fprintf(stderr, "Error: chunk index count mismatch: expected %u, wrote %u\n", index_count, index_cursor);
        for (uint32_t i = 0; i < dem_count; i++) free(windows[i].pixels);
        free(windows); free(used); free(max_y); free(max_x); free(min_y); free(min_x);
        free(sample_py); free(sample_px); free(sample_sources); free(indices); free(positions); free(vertices);
        return false;
    }

    char path[DC_UTILS_FILEPATH_BUFFER_SIZE];
    snprintf(path, sizeof(path), "%s/%s", output_dir, tile->acChunkFile);
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Error: could not open chunk for writing: %s\n", path);
        for (uint32_t i = 0; i < dem_count; i++) free(windows[i].pixels);
        free(windows); free(used); free(max_y); free(max_x); free(min_y); free(min_x);
        free(sample_py); free(sample_px); free(sample_sources); free(indices); free(positions); free(vertices);
        return false;
    }

    plPlanetChunkHeader header = {0};
    header.uMagic = PL_PLANET_CHUNK_MAGIC;
    header.uVersionMajor = PL_PLANET_CHUNK_VERSION_MAJOR;
    header.uVersionMinor = PL_PLANET_CHUNK_VERSION_MINOR;
    header.uHeaderSize = sizeof(header);
    header.uVertexSize = sizeof(plPlanetVertex);
    header.uFlags = PL_PLANET_CHUNK_FLAGS_HAS_SKIRTS;
    header.iFace = tile->tAddress.tFace;
    header.uLod = tile->tAddress.uLod;
    header.uX = tile->tAddress.uX;
    header.uY = tile->tAddress.uY;
    header.uSourceIndex = tile->uSourceIndex;
    header.uTileSize = tile_size;
    header.uVertexCount = vertex_count;
    header.uIndexCount = index_count;
    header.dRadius = radius;
    header.dGeometricError = tile->dGeometricError;
    header.dMinHeight = min_elevation == DBL_MAX ? 0.0 : min_elevation;
    header.dMaxHeight = max_elevation == -DBL_MAX ? 0.0 : max_elevation;
    header.tMinBound = min_bound;
    header.tMaxBound = max_bound;
    header.tMinBoundFlat = min_bound_flat;
    header.tMaxBoundFlat = max_bound_flat;
    header.ulVertexDataOffset = sizeof(header);
    header.ulIndexDataOffset = header.ulVertexDataOffset + (uint64_t)vertex_count * sizeof(plPlanetVertex);

    fwrite(&header, 1, sizeof(header), file);
    fwrite(vertices, 1, (size_t)vertex_count * sizeof(plPlanetVertex), file);
    fwrite(indices, 1, (size_t)index_count * sizeof(uint32_t), file);
    long file_size = ftell(file);
    fclose(file);

    tile->dMinHeight = header.dMinHeight;
    tile->dMaxHeight = header.dMaxHeight;
    tile->ulByteOffset = 0;
    tile->ulByteSize = file_size > 0 ? (uint64_t)file_size : 0u;

    for (uint32_t i = 0; i < dem_count; i++)
        free(windows[i].pixels);
    free(windows);
    free(used);
    free(max_y);
    free(max_x);
    free(min_y);
    free(min_x);
    free(sample_py);
    free(sample_px);
    free(sample_sources);
    free(indices);
    free(positions);
    free(vertices);
    return true;
}

//-----------------------------------------------------------------------------
// [SECTION] JSON writing
//-----------------------------------------------------------------------------

static bool _write_manifest_json(const char *output_dir, const char *prefix, const plPlanetManifest *manifest, const DcPlanetDemInfo *dem_infos, uint32_t dem_count, double radius, uint32_t tile_size, uint8_t max_lod, bool write_payloads, DcPlanetHeightMode height_mode) {
    char path[DC_UTILS_FILEPATH_BUFFER_SIZE];
    snprintf(path, sizeof(path), "%s/%s.planet.json", output_dir, prefix);

    plJsonObject *root = pl_json_new_root_object("root");
    pl_json_add_string_member(root, "schema", "planet");
    pl_json_add_int_member(root, "version", 1);
    pl_json_add_double_member(root, "radius", radius);
    pl_json_add_int_member(root, "tile_size", (int)tile_size);
    pl_json_add_int_member(root, "max_lod", (int)max_lod);
    pl_json_add_string_member(root, "tiling", "cube_sphere");
    pl_json_add_bool_member(root, "payloads", write_payloads);
    pl_json_add_string_member(root, "payload_format", "p2c");
    pl_json_add_string_member(root, "height_mode",
        height_mode == DC_PLANET_HEIGHT_MODE_RADIAL ? "radial" :
        height_mode == DC_PLANET_HEIGHT_MODE_OFFSET ? "offset" : "auto");

    plJsonObject *sources = pl_json_add_member_array(root, "sources", dem_count);
    for (uint32_t i = 0; i < dem_count; i++) {
        const plPlanetSourceRecord *source = _ext_planet->get_source(manifest, i);
        const DcPlanetDemInfo *dem = &dem_infos[i];
        plJsonObject *obj = pl_json_member_by_index(sources, i);
        pl_json_add_int_member(obj, "index", (int)i);
        pl_json_add_string_member(obj, "name", source->acName);
        pl_json_add_string_member(obj, "path", source->acPath);
        pl_json_add_int_member(obj, "flags", source->tFlags);
        pl_json_add_int_member(obj, "priority", source->uPriority);
        pl_json_add_double_member(obj, "native_meters_per_pixel", source->dNativeMetersPerPixel);
        pl_json_add_int_member(obj, "width", (int)dem->width);
        pl_json_add_int_member(obj, "height", (int)dem->height);
        pl_json_add_double_member(obj, "min_height", source->dMinHeight);
        pl_json_add_double_member(obj, "max_height", source->dMaxHeight);

        plJsonObject *footprint = pl_json_add_member(obj, "footprint");
        pl_json_add_double_member(footprint, "min_latitude", source->dMinLatitude);
        pl_json_add_double_member(footprint, "max_latitude", source->dMaxLatitude);
        pl_json_add_double_member(footprint, "min_longitude", source->dMinLongitude);
        pl_json_add_double_member(footprint, "max_longitude", source->dMaxLongitude);

        plJsonObject *projection = pl_json_add_member(obj, "projection");
        if (dem->projection_type == DC_PLANET_DEM_PROJECTION_CYLINDRICAL) {
            pl_json_add_string_member(projection, "type", "cylindrical");
            pl_json_add_bool_member(projection, "angular", dem->cylindrical.angular);
            pl_json_add_double_member(projection, "angular_to_degrees", dem->cylindrical.angular_to_degrees);
            pl_json_add_double_member(projection, "center_latitude", dem->cylindrical.center_latitude);
            pl_json_add_double_member(projection, "center_longitude", dem->cylindrical.center_longitude);
            pl_json_add_double_member(projection, "standard_parallel", dem->cylindrical.standard_parallel);
            pl_json_add_double_member(projection, "false_easting", dem->cylindrical.false_easting);
            pl_json_add_double_member(projection, "false_northing", dem->cylindrical.false_northing);
        } else {
            pl_json_add_string_member(projection, "type", "polar_stereographic");
            pl_json_add_double_member(projection, "latitude_of_origin", dem->polar.latitude_of_origin);
            pl_json_add_double_member(projection, "longitude_of_origin", dem->polar.longitude_of_origin);
            pl_json_add_double_member(projection, "scale_factor", dem->polar.scale_factor);
            pl_json_add_double_member(projection, "false_easting", dem->polar.false_easting);
            pl_json_add_double_member(projection, "false_northing", dem->polar.false_northing);
        }
    }

    uint32_t tile_count = _ext_planet->get_tile_count(manifest);
    plJsonObject *tiles = pl_json_add_member_array(root, "tiles", tile_count);
    for (uint32_t i = 0; i < tile_count; i++) {
        const plPlanetTileRecord *tile = _ext_planet->get_tile(manifest, i);
        plJsonObject *obj = pl_json_member_by_index(tiles, i);
        pl_json_add_int_member(obj, "face", tile->tAddress.tFace);
        pl_json_add_string_member(obj, "face_name", _face_name(tile->tAddress.tFace));
        pl_json_add_int_member(obj, "lod", tile->tAddress.uLod);
        pl_json_add_int_member(obj, "x", (int)tile->tAddress.uX);
        pl_json_add_int_member(obj, "y", (int)tile->tAddress.uY);
        pl_json_add_int_member(obj, "source", (int)tile->uSourceIndex);
        pl_json_add_double_member(obj, "geometric_error", tile->dGeometricError);
        pl_json_add_double_member(obj, "min_height", tile->dMinHeight);
        pl_json_add_double_member(obj, "max_height", tile->dMaxHeight);
        pl_json_add_string_member(obj, "file", tile->acChunkFile);
        pl_json_add_uint_member(obj, "byte_offset", (uint32_t)tile->ulByteOffset);
        pl_json_add_uint_member(obj, "byte_size", (uint32_t)tile->ulByteSize);
    }

    uint32_t buf_size = 0;
    pl_write_json(root, NULL, &buf_size);
    char *json_buf = (char *)malloc(buf_size + 1u);
    if (!json_buf) {
        pl_unload_json(&root);
        fprintf(stderr, "Error: failed to allocate JSON buffer\n");
        return false;
    }
    pl_write_json(root, json_buf, &buf_size);
    json_buf[buf_size] = '\0';
    pl_unload_json(&root);

    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Error: could not write manifest: %s\n", path);
        free(json_buf);
        return false;
    }
    fwrite(json_buf, 1, buf_size, file);
    fclose(file);
    free(json_buf);

    printf("Manifest: %s\n", path);
    return true;
}
