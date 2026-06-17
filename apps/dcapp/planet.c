#include "planet.h"

#include "geo.h"
#include "utils/file.h"
#include "utils/log.h"
#include "utils/string.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static void _planet_ensure_initialized(_AppData *app_data);
static bool _planet_resolve_file_path(_AppData *app_data, const char *path, char *out, size_t out_size);
static bool _planet_load_manifest_radius(const char *manifest_path, double *out_radius);
static bool _planet_file_path_to_vfs(_AppData *app_data, const char *path, char *out, size_t out_size);
static DcAppPlanetViewHandle _planet_create_view(_AppData *app_data, DcAppPlanetHandle planet, DcAppPlanetCrs crs, uint32_t width, uint32_t height);
static void _planet_update_breadcrumbs(DcAppPlanetBreadcrumbsHandle breadcrumbs, DcAppPlanetHandle planet, DcAppVec3 position);
static float _planet_breadcrumbs_distance(DcAppPlanetHandle planet, DcAppPlanetCrs crs, DcAppVec3 a, DcAppVec3 b);

// connects the public planet api table to the shared planet subsystem.
static const DcAppPlanetApi dc_app_planet_interface = {
    .get_planet_by_id       = dc_app_planet_get_planet_by_id,
    .create_planet          = dc_app_planet_create_planet,
    .create_planet_with_id  = dc_app_planet_create_planet_with_id,
    .set_texture_geodetic   = dc_app_planet_set_texture_geodetic,
    .set_texture_cartesian  = dc_app_planet_set_texture_cartesian,
    .create_geodetic_view   = dc_app_planet_create_geodetic_view,
    .create_cartesian_view  = dc_app_planet_create_cartesian_view,
    .set_view_shaders       = dc_app_planet_set_view_shaders,
    .create_breadcrumbs     = dc_app_planet_create_breadcrumbs,
    .update_breadcrumbs_geodetic  = dc_app_planet_update_breadcrumbs_geodetic,
    .update_breadcrumbs_cartesian = dc_app_planet_update_breadcrumbs_cartesian,
    .clear_breadcrumbs      = dc_app_planet_clear_breadcrumbs,
    .get_breadcrumbs_points = dc_app_planet_get_breadcrumbs_points,
};

const DcAppPlanetApi *dc_app_planet_api(void) {
    return &dc_app_planet_interface;
}

DcAppPlanetHandle dc_app_planet_get_planet_by_id(_AppData *app_data, const char *id) {
    if (!app_data || !id || id[0] == '\0') return NULL;

    for (int i = 0; i < sbcount(app_data->sb_planet_handles); i++) {
        DcAppPlanetHandle planet = app_data->sb_planet_handles[i];
        if (planet && planet->id && strcmp(planet->id, id) == 0) return planet;
    }

    return NULL;
}

DcAppPlanetHandle dc_app_planet_create_planet(_AppData *app_data, DcAppPlanetCreateInfo info) {
    if (!app_data || !info.data_path || info.data_path[0] == '\0') return NULL;

    // initializes the planet extension on first use.
    _planet_ensure_initialized(app_data);
    if (sbcount(app_data->sb_planets) > UINT8_MAX) {
        DC_LOG_ERROR("Planet", "Too many planets; dcapp supports at most %u planet handles", UINT8_MAX);
        return NULL;
    }

    char manifest_path[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    if (!_planet_resolve_file_path(app_data, info.data_path, manifest_path, sizeof(manifest_path))) {
        DC_LOG_ERROR("Planet", "Failed to resolve planet manifest path: %s", info.data_path);
        return NULL;
    }

    double radius = 0.0;
    if (!_planet_load_manifest_radius(manifest_path, &radius)) {
        DC_LOG_ERROR("Planet", "Invalid planet manifest: %s", manifest_path);
        return NULL;
    }

    if (info.mesh_cache_size > 0) {
        DC_LOG_WARN("Planet", "mesh_cache_size is ignored by the canonical planet renderer; configure the planet extension GPU cache instead");
    }

    plPlanetInit planet_init = {
        .pcManifestPath = manifest_path,
        .bValidatePayloads = true,
    };

    plCommandBuffer *cmd_buf = _ext_starter->get_temporary_command_buffer();
    plPlanet *planet = _ext_planet->create_planet(cmd_buf, planet_init);
    _ext_starter->submit_temporary_command_buffer(cmd_buf);
    if (!planet) return NULL;

    DcAppPlanetHandle handle = (DcAppPlanetHandle)PL_ALLOC(sizeof(*handle));
    memset(handle, 0, sizeof(*handle));
    handle->app_data = app_data;
    handle->planet = planet;
    handle->radius = radius;
    // stores crs helpers so xml and logic share the same conversions.
    handle->geodetic_crs = dc_geo_create_crs_geodetic(radius);
    handle->cartesian_crs = dc_geo_create_crs_cartesian(radius);
    handle->polar_crs = dc_geo_create_crs_polar_stereographic(radius, -90.0, 0.0);
    sbpush(app_data->sb_planets, planet);
    handle->index = (uint8_t)(sbcount(app_data->sb_planets) - 1);
    sbpush(app_data->sb_planet_handles, handle);
    return handle;
}

DcAppPlanetHandle dc_app_planet_create_planet_with_id(_AppData *app_data, const char *id, DcAppPlanetCreateInfo info) {
    if (!app_data || !id || id[0] == '\0') return NULL;
    if (dc_app_planet_get_planet_by_id(app_data, id)) {
        DC_LOG_ERROR("Planet", "Planet id already exists: %s", id);
        return NULL;
    }

    DcAppPlanetHandle planet = dc_app_planet_create_planet(app_data, info);
    if (planet) {
        size_t len = strlen(id) + 1;
        planet->id = (char *)PL_ALLOC(len);
        memcpy(planet->id, id, len);
    }
    return planet;
}

bool dc_app_planet_set_texture_geodetic(_AppData *app_data, DcAppPlanetHandle planet, const char *path, double lat, double lon, float meters_per_pixel) {
    (void)app_data;
    (void)path;
    (void)lat;
    (void)lon;
    (void)meters_per_pixel;
    return planet && planet->planet;
}

bool dc_app_planet_set_texture_cartesian(_AppData *app_data, DcAppPlanetHandle planet, const char *path, DcAppVec3 position, float meters_per_pixel) {
    (void)app_data;
    (void)path;
    (void)position;
    (void)meters_per_pixel;
    return planet && planet->planet;
}

DcAppPlanetViewHandle dc_app_planet_create_geodetic_view(_AppData *app_data, DcAppPlanetHandle planet, uint32_t width, uint32_t height) {
    return _planet_create_view(app_data, planet, DC_APP_PLANET_CRS_GEODETIC, width, height);
}

DcAppPlanetViewHandle dc_app_planet_create_cartesian_view(_AppData *app_data, DcAppPlanetHandle planet, uint32_t width, uint32_t height) {
    return _planet_create_view(app_data, planet, DC_APP_PLANET_CRS_CARTESIAN, width, height);
}

bool dc_app_planet_set_view_shaders(DcAppPlanetViewHandle view, const char *vertex_shader, const char *fragment_shader) {
    if (!view || !view->view) return false;

    char vertex_vfs[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    char fragment_vfs[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    const char *vertex_path = NULL;
    const char *fragment_path = NULL;

    if (vertex_shader && vertex_shader[0] != '\0') {
        if (!_planet_file_path_to_vfs(view->app_data, vertex_shader, vertex_vfs, sizeof(vertex_vfs))) return false;
        vertex_path = vertex_vfs;
    }
    if (fragment_shader && fragment_shader[0] != '\0') {
        if (!_planet_file_path_to_vfs(view->app_data, fragment_shader, fragment_vfs, sizeof(fragment_vfs))) return false;
        fragment_path = fragment_vfs;
    }

    _ext_planet->set_shaders(view->view, vertex_path, fragment_path);
    return true;
}

DcAppPlanetBreadcrumbsHandle dc_app_planet_create_breadcrumbs(_AppData *app_data, DcAppPlanetCrs crs, uint32_t max_points, float point_spacing) {
    if (!app_data) return NULL;
    if (crs != DC_APP_PLANET_CRS_GEODETIC && crs != DC_APP_PLANET_CRS_CARTESIAN) return NULL;
    if (max_points < 2) max_points = 2;
    if (point_spacing < 0.0f) point_spacing = 0.0f;

    DcAppPlanetBreadcrumbsHandle breadcrumbs = (DcAppPlanetBreadcrumbsHandle)PL_ALLOC(sizeof(*breadcrumbs));
    if (!breadcrumbs) return NULL;
    memset(breadcrumbs, 0, sizeof(*breadcrumbs));
    breadcrumbs->app_data = app_data;
    breadcrumbs->crs = crs;
    breadcrumbs->max_points = max_points;
    breadcrumbs->point_spacing = point_spacing;
    sbpush(app_data->sb_planet_breadcrumbs, breadcrumbs);
    return breadcrumbs;
}

void dc_app_planet_update_breadcrumbs_geodetic(DcAppPlanetBreadcrumbsHandle breadcrumbs, DcAppPlanetHandle planet, DcAppVec3 position) {
    if (!breadcrumbs || breadcrumbs->crs != DC_APP_PLANET_CRS_GEODETIC || !planet) return;
    _planet_update_breadcrumbs(breadcrumbs, planet, position);
}

void dc_app_planet_update_breadcrumbs_cartesian(DcAppPlanetBreadcrumbsHandle breadcrumbs, DcAppVec3 position) {
    if (!breadcrumbs || breadcrumbs->crs != DC_APP_PLANET_CRS_CARTESIAN) return;
    _planet_update_breadcrumbs(breadcrumbs, NULL, position);
}

void dc_app_planet_clear_breadcrumbs(DcAppPlanetBreadcrumbsHandle breadcrumbs) {
    if (!breadcrumbs) return;
    sbclear(breadcrumbs->sb_points);
}

DcAppPlanetBreadcrumbsPoints dc_app_planet_get_breadcrumbs_points(DcAppPlanetBreadcrumbsHandle breadcrumbs) {
    if (!breadcrumbs) return (DcAppPlanetBreadcrumbsPoints){0};
    return (DcAppPlanetBreadcrumbsPoints){
        .points = breadcrumbs->sb_points,
        .count = (uint32_t)sbcount(breadcrumbs->sb_points),
        .crs = breadcrumbs->crs,
    };
}

void dc_app_planet_free_wrappers(_AppData *app_data) {
    if (!app_data) return;

    for (int i = 0; i < sbcount(app_data->sb_planet_breadcrumbs); i++) {
        DcAppPlanetBreadcrumbsHandle breadcrumbs = app_data->sb_planet_breadcrumbs[i];
        if (!breadcrumbs) continue;
        sbfree(breadcrumbs->sb_points);
        PL_FREE(breadcrumbs);
    }
    sbfree(app_data->sb_planet_breadcrumbs);

    // cleans up every planet view created by the shared planet subsystem.
    for (int i = 0; i < sbcount(app_data->sb_planet_view_handles); i++) {
        DcAppPlanetViewHandle view = app_data->sb_planet_view_handles[i];
        if (!view) continue;
        if (view->view) _ext_planet->cleanup_view(view->view);
        PL_FREE(view);
    }
    sbfree(app_data->sb_planet_view_handles);

    // cleans up every planet created by the shared planet subsystem.
    for (int i = 0; i < sbcount(app_data->sb_planet_handles); i++) {
        DcAppPlanetHandle planet = app_data->sb_planet_handles[i];
        if (!planet) continue;
        if (planet->planet) _ext_planet->cleanup_planet(planet->planet);
        if (planet->id) PL_FREE(planet->id);
        PL_FREE(planet);
    }
    sbfree(app_data->sb_planet_handles);
    sbfree(app_data->sb_planet_views);
    sbfree(app_data->sb_planets);
}

plPlanet *dc_app_planet_pl(DcAppPlanetHandle planet) {
    return planet ? planet->planet : NULL;
}

plPlanetView *dc_app_planet_view_pl(DcAppPlanetViewHandle view) {
    return view ? view->view : NULL;
}

DcAppPlanetCrs dc_app_planet_view_crs(DcAppPlanetViewHandle view) {
    return view ? view->crs : DC_APP_PLANET_CRS_UNDEFINED;
}

DcAppPlanetHandle dc_app_planet_view_planet(DcAppPlanetViewHandle view) {
    return view ? view->planet : NULL;
}

static void _planet_update_breadcrumbs(DcAppPlanetBreadcrumbsHandle breadcrumbs, DcAppPlanetHandle planet, DcAppVec3 position) {
    if (!isfinite(position.x) || !isfinite(position.y) || !isfinite(position.z)) return;

    int point_count = sbcount(breadcrumbs->sb_points);
    if (point_count == 0 ||
        _planet_breadcrumbs_distance(planet, breadcrumbs->crs, breadcrumbs->sb_points[point_count - 1], position) >= breadcrumbs->point_spacing) {
        sbpush(breadcrumbs->sb_points, position);
    }

    point_count = sbcount(breadcrumbs->sb_points);
    if (point_count > (int)breadcrumbs->max_points) {
        sbshiftn(breadcrumbs->sb_points, point_count - (int)breadcrumbs->max_points);
    }
}

static float _planet_breadcrumbs_distance(DcAppPlanetHandle planet, DcAppPlanetCrs crs, DcAppVec3 a, DcAppVec3 b) {
    plVec3 pa = {a.x, a.y, a.z};
    plVec3 pb = {b.x, b.y, b.z};

    if (crs == DC_APP_PLANET_CRS_GEODETIC && planet) {
        plVec3 ca = {0};
        plVec3 cb = {0};
        dc_geo_geodetic_to_cartesian(&planet->geodetic_crs, &planet->cartesian_crs, &pa, &ca, 1);
        dc_geo_geodetic_to_cartesian(&planet->geodetic_crs, &planet->cartesian_crs, &pb, &cb, 1);
        pa = ca;
        pb = cb;
    }

    float dx = pb.x - pa.x;
    float dy = pb.y - pa.y;
    float dz = pb.z - pa.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void _planet_ensure_initialized(_AppData *app_data) {
    if (!app_data || app_data->planet_ext_initialized) return;

    plPlanetExtInit init = {0};
    init.ptDevice = _ext_starter->get_device();
    init.uGpuCacheSize = 1024u * 1024u * 1024u;
    _ext_planet->initialize(init);
    app_data->planet_ext_initialized = true;

    // keeps xml lookup arrays in their initialized one-based shape.
    sbpush(app_data->sb_planets, NULL);
    sbpush(app_data->sb_planet_views, NULL);
}

static bool _planet_resolve_file_path(_AppData *app_data, const char *path, char *out, size_t out_size) {
    if (!path || path[0] == '\0' || !out || out_size == 0) return false;

    char cleaned[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    strncpy(cleaned, path, sizeof(cleaned) - 1);
    dc_utils_trim_whitespace_inplace(cleaned);
    if (cleaned[0] == '\0') return false;

    if (dc_utils_is_relative_path(cleaned)) {
        const char *base_dir = app_data && app_data->config ? app_data->config->config_dir_path : NULL;
        if (!base_dir || base_dir[0] == '\0') return false;
        char joined[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
        if (dc_utils_join_paths(base_dir, cleaned, joined, sizeof(joined)) != 0) return false;
        return dc_utils_canonicalize_path(joined, out, out_size) == 0;
    }

    return dc_utils_canonicalize_path(cleaned, out, out_size) == 0;
}

static bool _planet_load_manifest_radius(const char *manifest_path, double *out_radius) {
    if (out_radius) *out_radius = 0.0;
    if (!manifest_path || !out_radius) return false;

    plPlanetManifest *manifest = _ext_planet->load_manifest_json(manifest_path, false);
    if (!manifest) return false;

    const plPlanetManifestInit *init = _ext_planet->get_manifest_init(manifest);
    const double radius = init ? init->dRadius : 0.0;
    _ext_planet->cleanup_manifest(manifest);

    if (radius <= 0.0) return false;
    *out_radius = radius;
    return true;
}

static bool _planet_file_path_to_vfs(_AppData *app_data, const char *path, char *out, size_t out_size) {
    if (!app_data || !path || path[0] == '\0' || !out || out_size == 0) return false;

    char cleaned[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    strncpy(cleaned, path, sizeof(cleaned) - 1);
    dc_utils_trim_whitespace_inplace(cleaned);
    if (cleaned[0] == '\0') return false;

    if (cleaned[0] == '/' && _ext_vfs->does_file_exist(cleaned)) {
        strncpy(out, cleaned, out_size - 1);
        out[out_size - 1] = '\0';
        return true;
    }

    char abs_path[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    if (dc_utils_is_relative_path(cleaned)) {
        const char *base_dir = app_data->config ? app_data->config->config_dir_path : NULL;
        if (!base_dir || base_dir[0] == '\0') return false;
        char joined[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
        if (dc_utils_join_paths(base_dir, cleaned, joined, sizeof(joined)) != 0) return false;
        if (dc_utils_canonicalize_path(joined, abs_path, sizeof(abs_path)) != 0) return false;
    } else if (dc_utils_canonicalize_path(cleaned, abs_path, sizeof(abs_path)) != 0) {
        return false;
    }

    char dir[DC_UTILS_FILEPATH_BUFFER_SIZE] = {0};
    dc_utils_get_directory(abs_path, dir, sizeof(dir));

    char hash[32] = {0};
    dc_utils_string_to_hash(dir, hash, sizeof(hash));

    char vfs_mount[33] = {0};
    snprintf(vfs_mount, sizeof(vfs_mount), "/%s", hash);
    _ext_vfs->mount_directory(vfs_mount, dir, PL_VFS_MOUNT_FLAGS_NONE);

    const char *fslash = strrchr(abs_path, '/');
    const char *bslash = strrchr(abs_path, '\\');
    const char *separator = fslash;
    if (!separator || (bslash && bslash > separator)) separator = bslash;
    const char *filename = separator ? separator + 1 : abs_path;
    snprintf(out, out_size, "%s/%s", vfs_mount, filename);
    return _ext_vfs->does_file_exist(out);
}

static DcAppPlanetViewHandle _planet_create_view(_AppData *app_data, DcAppPlanetHandle planet, DcAppPlanetCrs crs, uint32_t width, uint32_t height) {
    if (!app_data || !planet || !planet->planet) return NULL;

    _planet_ensure_initialized(app_data);
    if (sbcount(app_data->sb_planet_views) > UINT8_MAX) {
        DC_LOG_ERROR("PlanetView", "Too many planet views; dcapp supports at most %u planet view handles", UINT8_MAX);
        return NULL;
    }

    plPlanetViewInit view_init = {0};
    view_init.uOutputWidth = width > 0 ? width : 1024;
    view_init.uOutputHeight = height > 0 ? height : 1024;

    plCommandBuffer *cmd_buf = _ext_starter->get_temporary_command_buffer();
    plPlanetView *view = _ext_planet->create_view(planet->planet, cmd_buf, view_init);
    _ext_starter->submit_temporary_command_buffer(cmd_buf);
    if (!view) return NULL;

    DcAppPlanetViewHandle handle = (DcAppPlanetViewHandle)PL_ALLOC(sizeof(*handle));
    memset(handle, 0, sizeof(*handle));
    handle->app_data = app_data;
    handle->planet = planet;
    handle->view = view;
    handle->crs = crs;
    handle->width = view_init.uOutputWidth;
    handle->height = view_init.uOutputHeight;
    sbpush(app_data->sb_planet_views, view);
    handle->index = (uint8_t)(sbcount(app_data->sb_planet_views) - 1);
    sbpush(app_data->sb_planet_view_handles, handle);
    return handle;
}
