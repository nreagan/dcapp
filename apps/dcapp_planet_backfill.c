/*
   dcapp_planet_backfill.c

   Pilotlight app that generates a planet reference spheroid mesh from
   preprocessed .planet.json metadata.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pl.h"

#include "../src/geo.h"
#include "../src/utils/file.h"

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PL_ALLOC(x) _ext_memory->tracked_realloc(NULL, (x), __FILE__, __LINE__)
#define PL_FREE(x) _ext_memory->tracked_realloc((x), 0, __FILE__, __LINE__)
#define BACKFILL_SEGMENTS 384u

typedef struct DcPlanetMeshVertex {
    float position[3];
    float normal[3];
} DcPlanetMeshVertex;

typedef struct DcPlanetBackfillInfo {
    double radius;
    double meters_per_pixel;
    uint32_t tile_size;
    uint32_t tile_count;
    uint32_t cols;
    uint32_t rows;
    double min_x;
    double max_x;
    double min_y;
    double max_y;
    DcGeoCrsPolarStereo polar_crs;
    bool legacy_projected_origin;
} DcPlanetBackfillInfo;

typedef struct AppData {
    // CLI inputs
    const char *planet_data;
    const char *output;
    char default_output[DC_UTILS_FILEPATH_BUFFER_SIZE];

    // Mesh generation
    uint32_t segments;
    double overlap_meters;
    DcPlanetBackfillInfo info;

    // Run state
    bool done;
} AppData;

static const plIOI *_ext_ioi = NULL;
static const plMemoryI *_ext_memory = NULL;

PL_EXPORT void *pl_app_load(plApiRegistryI *api_registry, AppData *app);
PL_EXPORT void  pl_app_update(AppData *app);
PL_EXPORT void  pl_app_shutdown(AppData *app);

// Init and setup
static void _show_help(void);
static bool _load_planet_info(AppData *app);

// Argument parsing
static bool _parse_args(int argc, char **argv, AppData *app);

// Mesh output
static bool _write_backfill_mesh(AppData *app);

PL_EXPORT void *pl_app_load(plApiRegistryI *api_registry, AppData *app) {
    // Hot reload: refresh API pointers, keep existing app data.
    if (app) {
        _ext_ioi = pl_get_api_latest(api_registry, plIOI);
        _ext_memory = pl_get_api_latest(api_registry, plMemoryI);
        return app;
    }

    // Load only the extensions this utility needs.
    const plExtensionRegistryI *registry = pl_get_api_latest(api_registry, plExtensionRegistryI);
    registry->load("pl_unity_ext", NULL, NULL, true);
    registry->load("pl_platform_ext", "pl_load_platform_ext", "pl_unload_platform_ext", false);

    // Cache extension APIs.
    _ext_ioi = pl_get_api_latest(api_registry, plIOI);
    _ext_memory = pl_get_api_latest(api_registry, plMemoryI);

    app = (AppData *)PL_ALLOC(sizeof(AppData));
    if (!app)
        return NULL;
    memset(app, 0, sizeof(AppData));

    plIO *io = _ext_ioi->get_io();
    if (!_parse_args(io->iArgc - 3, io->apArgv + 3, app)) {
        io->bRunning = false;
        return app;
    }

    printf("========================================\n");
    printf("dcapp-planet-backfill\n");
    printf("========================================\n");
    printf("Input: %s\n", app->planet_data);
    printf("Output: %s\n", app->output);
    printf("Segments: %u\n", app->segments);

    if (!_load_planet_info(app)) {
        app->done = true;
        io->bRunning = false;
        return app;
    }

    app->overlap_meters = fmax(app->info.meters_per_pixel * 4.0,
                               (2.0 * M_PI * app->info.radius / (double)app->segments) * 0.75);

    printf("Radius: %.1f m\n", app->info.radius);
    printf("Tile footprint: x %.1f..%.1f, y %.1f..%.1f\n",
           app->info.min_x, app->info.max_x, app->info.min_y, app->info.max_y);
    printf("Cutout overlap: %.1f m\n", app->overlap_meters);
    printf("========================================\n\n");

    char output_dir[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    if (dc_utils_get_directory(app->output, output_dir, sizeof(output_dir)) && output_dir[0])
        dc_utils_create_directory(output_dir);

    if (!_write_backfill_mesh(app))
        fprintf(stderr, "Error: failed to generate backfill mesh\n");

    app->done = true;
    io->bRunning = false;
    return app;
}

PL_EXPORT void pl_app_update(AppData *app) {
    (void)app;
    plIO *io = _ext_ioi->get_io();
    io->bRunning = false;
}

PL_EXPORT void pl_app_shutdown(AppData *app) {
    if (!app)
        return;
    PL_FREE(app);
}

static void _show_help(void) {
    printf("Usage: dcapp-planet-backfill <planet_json> [output_dcpm]\n");
    printf("\n");
    printf("Required:\n");
    printf("  <planet_json>           Preprocessed .planet.json chunk metadata\n");
    printf("\n");
    printf("Optional positional:\n");
    printf("  [output_dcpm]           Output .dcpm path (default: <planet_json-stem>_reference.dcpm)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help\n");
}

static bool _parse_args(int argc, char **argv, AppData *app) {
    // Defaults
    memset(app, 0, sizeof(*app));
    app->segments = BACKFILL_SEGMENTS;

    // First pass: collect raw option strings.
    const char *planet_data = NULL;
    const char *output = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            _show_help();
            return false;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            return false;
        } else if (!planet_data) {
            planet_data = argv[i];
        } else if (!output) {
            output = argv[i];
        } else {
            fprintf(stderr, "Error: unexpected argument: %s\n", argv[i]);
            return false;
        }
    }

    if (!planet_data) {
        fprintf(stderr, "Error: planet data is required\n");
        _show_help();
        return false;
    }
    if (!dc_utils_file_exists(planet_data)) {
        fprintf(stderr, "Error: input file not found: %s\n", planet_data);
        return false;
    }

    app->planet_data = planet_data;
    if (output) {
        app->output = output;
    } else {
        char dir[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
        dc_utils_get_directory(planet_data, dir, sizeof(dir));

        const char *fslash = strrchr(planet_data, '/');
        const char *bslash = strrchr(planet_data, '\\');
        const char *slash = (fslash > bslash) ? fslash : bslash;
        const char *name = slash ? slash + 1 : planet_data;

        char stem[512];
        strncpy(stem, name, sizeof(stem) - 1);
        stem[sizeof(stem) - 1] = '\0';

        const char *planet_suffix = ".planet.json";
        size_t stem_len = strlen(stem);
        size_t suffix_len = strlen(planet_suffix);
        if (stem_len > suffix_len && strcmp(stem + stem_len - suffix_len, planet_suffix) == 0) {
            stem[stem_len - suffix_len] = '\0';
        } else {
            char *dot = strrchr(stem, '.');
            if (dot)
                *dot = '\0';
        }

        char filename[640];
        snprintf(filename, sizeof(filename), "%s_reference.dcpm", stem);
        if (dir[0])
            dc_utils_join_paths(dir, filename, app->default_output, sizeof(app->default_output));
        else
            snprintf(app->default_output, sizeof(app->default_output), "%s", filename);
        app->output = app->default_output;
    }

    return true;
}

static bool _load_planet_info(AppData *app) {
    char *json_text = dc_utils_load_text_file(app->planet_data);
    if (!json_text) {
        fprintf(stderr, "Error: failed to load planet data: %s\n", app->planet_data);
        return false;
    }

    plJsonObject *root = NULL;
    if (!pl_load_json(json_text, &root)) {
        fprintf(stderr, "Error: failed to parse planet data: %s\n", app->planet_data);
        free(json_text);
        return false;
    }

    double radius = pl_json_double_member(root, "radius", 0.0);
    double meters_per_pixel = pl_json_double_member(root, "meters_per_pixel", 0.0);
    int tile_size = pl_json_int_member(root, "tile_size", 0);
    int cols = pl_json_int_member(root, "cols", 0);
    int rows = pl_json_int_member(root, "rows", 0);
    uint32_t tile_count = 0;
    plJsonObject *tile_array = pl_json_array_member(root, "tiles", &tile_count);

    if (radius <= 0.0 || meters_per_pixel <= 0.0 || tile_size <= 0 || cols <= 0 || rows <= 0 || !tile_array || tile_count == 0) {
        fprintf(stderr, "Error: invalid planet metadata: %s\n", app->planet_data);
        pl_unload_json(&root);
        free(json_text);
        return false;
    }
    uint64_t expected_tile_count = (uint64_t)cols * (uint64_t)rows;
    if (expected_tile_count > UINT32_MAX || tile_count != (uint32_t)expected_tile_count) {
        fprintf(stderr, "Error: planet tile count mismatch: cols=%d rows=%d tiles=%u\n", cols, rows, tile_count);
        pl_unload_json(&root);
        free(json_text);
        return false;
    }

    DcPlanetBackfillInfo *info = &app->info;
    memset(info, 0, sizeof(*info));
    info->radius = radius;
    info->meters_per_pixel = meters_per_pixel;
    info->tile_size = (uint32_t)tile_size;
    info->tile_count = tile_count;
    info->cols = (uint32_t)cols;
    info->rows = (uint32_t)rows;
    info->min_x = INFINITY;
    info->max_x = -INFINITY;
    info->min_y = INFINITY;
    info->max_y = -INFINITY;
    info->polar_crs = dc_geo_create_crs_polar_stereographic(radius, -90.0, 0.0);

    plJsonObject *projection_obj = pl_json_member(root, "projection");
    info->legacy_projected_origin = projection_obj == NULL;
    if (projection_obj) {
        char projection_type[64] = {0};
        pl_json_string_member(projection_obj, "type", projection_type, sizeof(projection_type));
        if (projection_type[0] && strcmp(projection_type, "polar_stereographic") != 0) {
            fprintf(stderr, "Error: unsupported planet projection '%s'\n", projection_type);
            pl_unload_json(&root);
            free(json_text);
            return false;
        }
        info->polar_crs.lat_origin =
            pl_json_double_member(projection_obj, "latitude_of_origin", info->polar_crs.lat_origin);
        info->polar_crs.lon_origin =
            pl_json_double_member(projection_obj, "longitude_of_origin", info->polar_crs.lon_origin);
        info->polar_crs.scale_factor =
            pl_json_double_member(projection_obj, "scale_factor", info->polar_crs.scale_factor);
        info->polar_crs.false_easting =
            pl_json_double_member(projection_obj, "false_easting", info->polar_crs.false_easting);
        info->polar_crs.false_northing =
            pl_json_double_member(projection_obj, "false_northing", info->polar_crs.false_northing);
    }
    if (fabs(info->polar_crs.lat_origin) < 45.0 || info->polar_crs.scale_factor <= 0.0) {
        fprintf(stderr, "Error: invalid polar stereographic projection in planet data\n");
        pl_unload_json(&root);
        free(json_text);
        return false;
    }

    DcGeoCrsGeodetic geodetic_crs = dc_geo_create_crs_geodetic(radius);
    const double half_tile = 0.5 * (double)tile_size * meters_per_pixel;
    double first_origin_x = 0.0;
    double first_origin_y = 0.0;
    for (uint32_t i = 0; i < tile_count; i++) {
        plJsonObject *tile_obj = pl_json_member_by_index(tile_array, i);
        double origin_x = 0.0;
        double origin_y = 0.0;
        bool have_origin = false;

        if (pl_json_member_exist(tile_obj, "originX") && pl_json_member_exist(tile_obj, "originY")) {
            origin_x = pl_json_double_member(tile_obj, "originX", 0.0);
            origin_y = pl_json_double_member(tile_obj, "originY", 0.0);
            have_origin = true;
        } else if (pl_json_member_exist(tile_obj, "lat") && pl_json_member_exist(tile_obj, "lon")) {
            plVec3d geodetic_in = {
                pl_json_double_member(tile_obj, "lat", 0.0),
                pl_json_double_member(tile_obj, "lon", 0.0),
                0.0
            };
            plVec2d polar_out;
            dc_geo_geodetic_to_polar_stereo_d(&geodetic_crs, &info->polar_crs, &geodetic_in, &polar_out, 1);
            if (info->legacy_projected_origin)
                polar_out.y = -polar_out.y;
            origin_x = polar_out.x;
            origin_y = polar_out.y;
            have_origin = true;
        }

        if (!have_origin) {
            fprintf(stderr, "Error: tile %u is missing originX/originY or lat/lon\n", i);
            pl_unload_json(&root);
            free(json_text);
            return false;
        }

        if (i == 0) {
            first_origin_x = origin_x;
            first_origin_y = origin_y;
        }
        info->min_x = fmin(info->min_x, origin_x - half_tile);
        info->max_x = fmax(info->max_x, origin_x + half_tile);
        info->min_y = fmin(info->min_y, origin_y - half_tile);
        info->max_y = fmax(info->max_y, origin_y + half_tile);
    }

    if (info->legacy_projected_origin) {
        info->min_x = first_origin_x - half_tile;
        info->max_x = info->min_x + (double)cols * (double)tile_size * meters_per_pixel;
        info->max_y = first_origin_y + half_tile;
        info->min_y = info->max_y - (double)rows * (double)tile_size * meters_per_pixel;
    }

    pl_unload_json(&root);
    free(json_text);
    return isfinite(info->min_x) && isfinite(info->max_x) && isfinite(info->min_y) && isfinite(info->max_y);
}

static bool _write_backfill_mesh(AppData *app) {
    DcPlanetBackfillInfo *info = &app->info;
    const uint32_t lon_segments = app->segments;
    const uint32_t lat_segments = app->segments / 2u;
    const size_t vertex_count = (size_t)(lat_segments + 1u) * (size_t)lon_segments;
    const size_t max_index_count = (size_t)lat_segments * (size_t)lon_segments * 6u;
    if (vertex_count > UINT32_MAX || max_index_count > UINT32_MAX) {
        fprintf(stderr, "Error: backfill mesh is too large\n");
        return false;
    }

    DcPlanetMeshVertex *vertices = (DcPlanetMeshVertex *)calloc(vertex_count, sizeof(DcPlanetMeshVertex));
    uint32_t *indices = (uint32_t *)malloc(max_index_count * sizeof(uint32_t));
    if (!vertices || !indices) {
        fprintf(stderr, "Error: failed to allocate backfill mesh\n");
        free(vertices);
        free(indices);
        return false;
    }

    const bool north = info->polar_crs.lat_origin > 0.0;
    const double scale_factor = info->polar_crs.scale_factor > 0.0 ? info->polar_crs.scale_factor : 1.0;
    const double lon_origin_rad = info->polar_crs.lon_origin * M_PI / 180.0;
    const double cut_min_x = info->min_x - app->overlap_meters;
    const double cut_max_x = info->max_x + app->overlap_meters;
    const double cut_min_y = info->min_y - app->overlap_meters;
    const double cut_max_y = info->max_y + app->overlap_meters;

    for (uint32_t lat_i = 0; lat_i <= lat_segments; lat_i++) {
        const double lat_rad = -M_PI * 0.5 + M_PI * (double)lat_i / (double)lat_segments;
        const double cos_lat = cos(lat_rad);
        const double sin_lat = sin(lat_rad);
        for (uint32_t lon_i = 0; lon_i < lon_segments; lon_i++) {
            const double lon_rad = 2.0 * M_PI * (double)lon_i / (double)lon_segments;
            const double nx = cos_lat * sin(lon_rad);
            const double ny = sin_lat;
            const double nz = -cos_lat * cos(lon_rad);
            const size_t vi = (size_t)lat_i * (size_t)lon_segments + (size_t)lon_i;
            vertices[vi].normal[0] = (float)nx;
            vertices[vi].normal[1] = (float)ny;
            vertices[vi].normal[2] = (float)nz;
            vertices[vi].position[0] = (float)(info->radius * nx);
            vertices[vi].position[1] = (float)(info->radius * ny);
            vertices[vi].position[2] = (float)(info->radius * nz);
        }
    }

    uint32_t index_count = 0;
#define PROJECT_SPHERE_POINT(LAT, LON, OUT_X, OUT_Y, OUT_OK) do { \
        double lat__ = (LAT); \
        double theta__ = (LON) - lon_origin_rad; \
        double rho__ = north \
            ? 2.0 * info->radius * scale_factor * tan(M_PI / 4.0 - 0.5 * lat__) \
            : 2.0 * info->radius * scale_factor * tan(M_PI / 4.0 + 0.5 * lat__); \
        (OUT_X) = info->polar_crs.false_easting + rho__ * sin(theta__); \
        (OUT_Y) = info->polar_crs.false_northing + (north ? -rho__ : rho__) * cos(theta__); \
        if (info->legacy_projected_origin) \
            (OUT_Y) = -(OUT_Y); \
        (OUT_OK) = isfinite(OUT_X) && isfinite(OUT_Y); \
    } while (0)
#define POINT_IN_CUTOUT(X, Y) ((X) >= cut_min_x && (X) <= cut_max_x && (Y) >= cut_min_y && (Y) <= cut_max_y)
#define TRI_TOUCHES_CUTOUT(RESULT, LAT0, LON0, LAT1, LON1, LAT2, LON2) do { \
        double px0__, py0__, px1__, py1__, px2__, py2__, pxc__, pyc__; \
        bool ok0__, ok1__, ok2__, okc__; \
        PROJECT_SPHERE_POINT((LAT0), (LON0), px0__, py0__, ok0__); \
        PROJECT_SPHERE_POINT((LAT1), (LON1), px1__, py1__, ok1__); \
        PROJECT_SPHERE_POINT((LAT2), (LON2), px2__, py2__, ok2__); \
        PROJECT_SPHERE_POINT(((LAT0) + (LAT1) + (LAT2)) / 3.0, ((LON0) + (LON1) + (LON2)) / 3.0, pxc__, pyc__, okc__); \
        (RESULT) = false; \
        if ((ok0__ && POINT_IN_CUTOUT(px0__, py0__)) || \
            (ok1__ && POINT_IN_CUTOUT(px1__, py1__)) || \
            (ok2__ && POINT_IN_CUTOUT(px2__, py2__)) || \
            (okc__ && POINT_IN_CUTOUT(pxc__, pyc__))) { \
            (RESULT) = true; \
        } \
    } while (0)
#define ADD_BACKFILL_TRI(A, B, C) do { \
        uint32_t a_ = (A); \
        uint32_t b_ = (B); \
        uint32_t c_ = (C); \
        double ax_ = vertices[a_].position[0], ay_ = vertices[a_].position[1], az_ = vertices[a_].position[2]; \
        double bx_ = vertices[b_].position[0], by_ = vertices[b_].position[1], bz_ = vertices[b_].position[2]; \
        double cx_ = vertices[c_].position[0], cy_ = vertices[c_].position[1], cz_ = vertices[c_].position[2]; \
        double ux_ = bx_ - ax_, uy_ = by_ - ay_, uz_ = bz_ - az_; \
        double vx_ = cx_ - ax_, vy_ = cy_ - ay_, vz_ = cz_ - az_; \
        double fx_ = uy_ * vz_ - uz_ * vy_; \
        double fy_ = uz_ * vx_ - ux_ * vz_; \
        double fz_ = ux_ * vy_ - uy_ * vx_; \
        double nx_ = vertices[a_].normal[0] + vertices[b_].normal[0] + vertices[c_].normal[0]; \
        double ny_ = vertices[a_].normal[1] + vertices[b_].normal[1] + vertices[c_].normal[1]; \
        double nz_ = vertices[a_].normal[2] + vertices[b_].normal[2] + vertices[c_].normal[2]; \
        double facing_ = fx_ * nx_ + fy_ * ny_ + fz_ * nz_; \
        if (facing_ < 0.0) { \
            uint32_t tmp_ = b_; \
            b_ = c_; \
            c_ = tmp_; \
        } \
        indices[index_count++] = a_; \
        indices[index_count++] = b_; \
        indices[index_count++] = c_; \
    } while (0)

    for (uint32_t lat_i = 0; lat_i < lat_segments; lat_i++) {
        const double lat0 = -M_PI * 0.5 + M_PI * (double)lat_i / (double)lat_segments;
        const double lat1 = -M_PI * 0.5 + M_PI * (double)(lat_i + 1u) / (double)lat_segments;
        for (uint32_t lon_i = 0; lon_i < lon_segments; lon_i++) {
            const uint32_t lon_next = (lon_i + 1u) % lon_segments;
            const double lon0 = 2.0 * M_PI * (double)lon_i / (double)lon_segments;
            const double lon1 = 2.0 * M_PI * (double)(lon_i + 1u) / (double)lon_segments;
            const uint32_t v00 = lat_i * lon_segments + lon_i;
            const uint32_t v01 = lat_i * lon_segments + lon_next;
            const uint32_t v10 = (lat_i + 1u) * lon_segments + lon_i;
            const uint32_t v11 = (lat_i + 1u) * lon_segments + lon_next;

            bool skip = false;
            if (lat_i == 0) {
                TRI_TOUCHES_CUTOUT(skip, lat0, lon0, lat1, lon0, lat1, lon1);
                if (!skip)
                    ADD_BACKFILL_TRI(v00, v10, v11);
            } else if (lat_i + 1u == lat_segments) {
                TRI_TOUCHES_CUTOUT(skip, lat0, lon0, lat1, lon0, lat0, lon1);
                if (!skip)
                    ADD_BACKFILL_TRI(v00, v10, v01);
            } else {
                TRI_TOUCHES_CUTOUT(skip, lat0, lon0, lat1, lon0, lat0, lon1);
                if (!skip)
                    ADD_BACKFILL_TRI(v00, v10, v01);
                TRI_TOUCHES_CUTOUT(skip, lat0, lon1, lat1, lon0, lat1, lon1);
                if (!skip)
                    ADD_BACKFILL_TRI(v01, v10, v11);
            }
        }
    }
#undef ADD_BACKFILL_TRI
#undef TRI_TOUCHES_CUTOUT
#undef POINT_IN_CUTOUT
#undef PROJECT_SPHERE_POINT

    const char magic[4] = {'D', 'C', 'P', 'M'};
    const uint32_t version = 1;
    const uint32_t vertex_count_u32 = (uint32_t)vertex_count;
    const uint32_t stride = (uint32_t)sizeof(DcPlanetMeshVertex);
    const uint32_t flags = 0;

    const size_t header_size = sizeof(magic) + sizeof(version) + sizeof(vertex_count_u32) +
                               sizeof(index_count) + sizeof(stride) + sizeof(flags);
    const size_t vertex_bytes = vertex_count * sizeof(DcPlanetMeshVertex);
    const size_t index_bytes = (size_t)index_count * sizeof(uint32_t);
    const size_t output_size = header_size + vertex_bytes + index_bytes;
    unsigned char *output_data = (unsigned char *)malloc(output_size);
    if (!output_data) {
        fprintf(stderr, "Error: failed to allocate backfill output buffer\n");
        free(vertices);
        free(indices);
        return false;
    }

    unsigned char *cursor = output_data;
    memcpy(cursor, magic, sizeof(magic)); cursor += sizeof(magic);
    memcpy(cursor, &version, sizeof(version)); cursor += sizeof(version);
    memcpy(cursor, &vertex_count_u32, sizeof(vertex_count_u32)); cursor += sizeof(vertex_count_u32);
    memcpy(cursor, &index_count, sizeof(index_count)); cursor += sizeof(index_count);
    memcpy(cursor, &stride, sizeof(stride)); cursor += sizeof(stride);
    memcpy(cursor, &flags, sizeof(flags)); cursor += sizeof(flags);
    memcpy(cursor, vertices, vertex_bytes); cursor += vertex_bytes;
    memcpy(cursor, indices, index_bytes);

    bool ok = dc_utils_write_binary_file(app->output, output_data, output_size);
    free(output_data);
    free(vertices);
    free(indices);

    if (!ok) {
        fprintf(stderr, "Error: failed to write complete backfill mesh: %s\n", app->output);
        return false;
    }

    printf("Backfill mesh triangles: %u\n", index_count / 3);
    printf("Done: %s\n", app->output);
    return true;
}
