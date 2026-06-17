/*
   dcapp_planet_snapshot.c

   Minimal GPU smoke/snapshot app for planet manifests.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pl.h"

#define PL_MATH_INCLUDE_FUNCTIONS
#include "pl_math.h"

#include "pl_camera_ext.h"
#include "pl_graphics_ext.h"
#include "pl_planet_ext.h"
#include "pl_shader_ext.h"
#include "pl_starter_ext.h"
#include "pl_vfs_ext.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define PL_ALLOC(x) _ext_memory->tracked_realloc(NULL, (x), __FILE__, __LINE__)
#define PL_FREE(x)  _ext_memory->tracked_realloc((x), 0, __FILE__, __LINE__)

typedef struct AppData
{
    const char* manifest_path;
    const char* output_path;
    const char* vertex_shader;
    const char* fragment_shader;

    uint32_t width;
    uint32_t height;
    uint32_t gpu_cache_mb;
    float    fov;
    float    lod_threshold;
    double   radius;

    bool has_eye;
    bool has_target;
    plVec3d eye;
    plVec3d target;

    plPlanetRenderFlags flags;

    plWindow* window;
    plPlanet* planet;
    plPlanetView* view;
    plBufferHandle readback_buffer;
    size_t readback_size;

    bool starter_initialized;
    bool shader_initialized;
    bool planet_initialized;
    bool done;
} AppData;

static const plIOI*       _ext_ioi = NULL;
static const plWindowI*   _ext_windows = NULL;
static const plGraphicsI* _ext_gfx = NULL;
static const plStarterI*  _ext_starter = NULL;
static const plShaderI*   _ext_shader = NULL;
static const plVfsI*      _ext_vfs = NULL;
static const plCameraI*   _ext_camera = NULL;
static const plPlanetI*  _ext_planet = NULL;
static const plMemoryI*   _ext_memory = NULL;

static void _show_help(void);
static bool _parse_args(int argc, char** argv, AppData* app);
static bool _parse_double(const char* text, double* out);
static bool _parse_uint(const char* text, uint32_t* out);

PL_EXPORT void*
pl_app_load(plApiRegistryI* api_registry, AppData* app)
{
    if(app)
    {
        _ext_ioi = pl_get_api_latest(api_registry, plIOI);
        _ext_windows = pl_get_api_latest(api_registry, plWindowI);
        _ext_gfx = pl_get_api_latest(api_registry, plGraphicsI);
        _ext_starter = pl_get_api_latest(api_registry, plStarterI);
        _ext_shader = pl_get_api_latest(api_registry, plShaderI);
        _ext_vfs = pl_get_api_latest(api_registry, plVfsI);
        _ext_camera = pl_get_api_latest(api_registry, plCameraI);
        _ext_planet = pl_get_api_latest(api_registry, plPlanetI);
        _ext_memory = pl_get_api_latest(api_registry, plMemoryI);
        return app;
    }

    const plExtensionRegistryI* extension_registry = pl_get_api_latest(api_registry, plExtensionRegistryI);
    extension_registry->load("pl_unity_ext", NULL, NULL, true);
    extension_registry->load("pl_platform_ext", "pl_load_platform_ext", "pl_unload_platform_ext", false);
    extension_registry->load("pl_planet_ext", NULL, NULL, true);

    _ext_ioi = pl_get_api_latest(api_registry, plIOI);
    _ext_windows = pl_get_api_latest(api_registry, plWindowI);
    _ext_gfx = pl_get_api_latest(api_registry, plGraphicsI);
    _ext_starter = pl_get_api_latest(api_registry, plStarterI);
    _ext_shader = pl_get_api_latest(api_registry, plShaderI);
    _ext_vfs = pl_get_api_latest(api_registry, plVfsI);
    _ext_camera = pl_get_api_latest(api_registry, plCameraI);
    _ext_planet = pl_get_api_latest(api_registry, plPlanetI);
    _ext_memory = pl_get_api_latest(api_registry, plMemoryI);

    plIO* io = _ext_ioi->get_io();
    app = (AppData*)PL_ALLOC(sizeof(AppData));
    if(!app)
    {
        io->bRunning = false;
        return NULL;
    }
    memset(app, 0, sizeof(*app));
    app->width = 1024;
    app->height = 768;
    app->fov = 45.0f;

    if(!_parse_args(io->iArgc - 3, io->apArgv + 3, app))
    {
        io->bRunning = false;
        return app;
    }

    _ext_vfs->mount_directory("/shaders-terrain", "../../shaders", PL_VFS_MOUNT_FLAGS_NONE);
    _ext_vfs->mount_directory("/shaders", "../shaders", PL_VFS_MOUNT_FLAGS_NONE);
    _ext_vfs->mount_directory("/shader-temp", "../shader-temp", PL_VFS_MOUNT_FLAGS_NONE);

    const plWindowDesc window_desc = {
        .pcTitle = "dcapp planet snapshot",
        .iXPos = 100,
        .iYPos = 100,
        .uWidth = app->width,
        .uHeight = app->height,
    };
    _ext_windows->create(window_desc, &app->window);
    _ext_windows->show(app->window);

    const plStarterInit starter_init = {
        .tFlags = (PL_STARTER_FLAGS_ALL_EXTENSIONS & ~PL_STARTER_FLAGS_SHADER_EXT) | PL_STARTER_FLAGS_VSYNC_OFF,
        .ptWindow = app->window,
    };
    _ext_starter->initialize(starter_init);
    app->starter_initialized = true;

    plShaderOptions shader_options = {0};
    shader_options.apcIncludeDirectories[0] = "/shaders/";
    shader_options.apcIncludeDirectories[1] = "/shaders-terrain/";
    shader_options.apcDirectories[0] = "/shaders/";
    shader_options.apcDirectories[1] = "/shaders-terrain/";
    shader_options.pcCacheOutputDirectory = "/shader-temp/";
    shader_options.tFlags = PL_SHADER_FLAGS_AUTO_OUTPUT | PL_SHADER_FLAGS_INCLUDE_DEBUG | PL_SHADER_FLAGS_ALWAYS_COMPILE;
    _ext_shader->initialize(&shader_options);
    app->shader_initialized = true;

    _ext_starter->finalize();

    plDevice* device = _ext_starter->get_device();
    _ext_planet->initialize((plPlanetExtInit){
        .ptDevice = device,
        .uGpuCacheSize = app->gpu_cache_mb ? app->gpu_cache_mb * 1024u * 1024u : 0u
    });
    app->planet_initialized = true;

    plPlanetManifest* manifest = _ext_planet->load_manifest_json(app->manifest_path, true);
    if(!manifest)
    {
        fprintf(stderr, "Error: failed to load planet manifest: %s\n", app->manifest_path);
        io->bRunning = false;
        return app;
    }

    const plPlanetManifestInit* manifest_init = _ext_planet->get_manifest_init(manifest);
    app->radius = manifest_init ? manifest_init->dRadius : 1737400.0;
    if(!app->has_eye)
        app->eye = (plVec3d){0.0, -app->radius * 3.0, 0.0};
    if(!app->has_target)
        app->target = (plVec3d){0.0, 0.0, 0.0};

    plCommandBuffer* cmd = _ext_starter->get_temporary_command_buffer();
    app->planet = _ext_planet->create_planet(cmd, (plPlanetInit){.ptManifest = manifest});
    _ext_starter->submit_temporary_command_buffer(cmd);
    if(!app->planet)
    {
        fprintf(stderr, "Error: failed to create planet GPU planet from manifest\n");
        io->bRunning = false;
        return app;
    }

    cmd = _ext_starter->get_temporary_command_buffer();
    app->view = _ext_planet->create_view(app->planet, cmd, (plPlanetViewInit){
        .uOutputWidth = app->width,
        .uOutputHeight = app->height,
        .pcVertexShader = app->vertex_shader,
        .pcFragmentShader = app->fragment_shader,
    });
    _ext_starter->submit_temporary_command_buffer(cmd);
    if(!app->view)
    {
        fprintf(stderr, "Error: failed to create planet view\n");
        io->bRunning = false;
        return app;
    }

    plPlanetViewRuntimeOptions view_options = _ext_planet->get_view_runtime_options(app->view);
    view_options.tFlags = app->flags;
    if(app->lod_threshold > 0.0f)
        view_options.fLodPixelThreshold = app->lod_threshold;
    _ext_planet->set_view_runtime_options(app->view, view_options);

    app->readback_size = (size_t)app->width * (size_t)app->height * 4u;
    const plBufferDesc readback_desc = {
        .tUsage = PL_BUFFER_USAGE_STAGING,
        .szByteSize = app->readback_size,
        .pcDebugName = "planet snapshot readback",
    };
    app->readback_buffer = _ext_gfx->create_buffer(device, &readback_desc, NULL);
    plBuffer* readback_buffer = _ext_gfx->get_buffer(device, app->readback_buffer);
    if(!readback_buffer)
    {
        io->bRunning = false;
        return app;
    }
    const plDeviceMemoryAllocation readback_allocation = _ext_gfx->allocate_memory(
        device,
        readback_buffer->tMemoryRequirements.ulSize,
        PL_MEMORY_FLAGS_HOST_VISIBLE | PL_MEMORY_FLAGS_HOST_COHERENT,
        readback_buffer->tMemoryRequirements.uMemoryTypeBits,
        "planet snapshot readback memory");
    _ext_gfx->bind_buffer_to_memory(device, app->readback_buffer, &readback_allocation);

    return app;
}

PL_EXPORT void
pl_app_update(AppData* app)
{
    if(!app || app->done)
        return;
    if(!_ext_starter->begin_frame())
        return;

    plCamera camera = {0};
    camera.tType = PL_CAMERA_TYPE_PERSPECTIVE_REVERSE_Z;
    camera.fFieldOfView = pl_radiansf(app->fov);
    camera.fAspectRatio = app->height > 0 ? (float)app->width / (float)app->height : 1.0f;
    camera.fNearZ = 1.0f;
    camera.fFarZ = (float)(app->radius * 20.0);
    camera.fWidth = (float)app->width;
    camera.fHeight = (float)app->height;
    _ext_camera->look_at(&camera, app->eye, app->target);
    _ext_camera->update(&camera);

    plCommandBuffer* cmd = _ext_starter->get_command_buffer();
    _ext_planet->render_view(app->view, &camera, cmd);
    _ext_starter->submit_command_buffer(cmd);
    _ext_starter->end_frame();
    _ext_gfx->flush_device(_ext_starter->get_device());

    plCommandBuffer* copy_cmd = _ext_starter->get_temporary_command_buffer();
    plBlitEncoder* blit = _ext_gfx->begin_blit_pass(copy_cmd);
    const plBufferImageCopy copy = {
        .uImageWidth = app->width,
        .uImageHeight = app->height,
        .uImageDepth = 1,
        .uLayerCount = 1,
        .tCurrentImageUsage = PL_TEXTURE_USAGE_SAMPLED,
    };
    _ext_gfx->copy_texture_to_buffer(blit, _ext_planet->get_view_output_texture(app->view), app->readback_buffer, 1, &copy);
    _ext_gfx->end_blit_pass(blit);
    _ext_starter->submit_temporary_command_buffer(copy_cmd);
    _ext_gfx->flush_device(_ext_starter->get_device());

    plBuffer* buffer = _ext_gfx->get_buffer(_ext_starter->get_device(), app->readback_buffer);
    const int stride = (int)app->width * 4;
    if(!buffer || !buffer->tMemoryAllocation.pHostMapped ||
       !stbi_write_png(app->output_path, (int)app->width, (int)app->height, 4, buffer->tMemoryAllocation.pHostMapped, stride))
    {
        fprintf(stderr, "Error: failed to write snapshot: %s\n", app->output_path);
    }
    else
    {
        const plPlanetRenderStats stats = _ext_planet->get_render_stats(app->planet);
        printf("Wrote planet snapshot: %s (%u drawn, %u resident, %u manifest, %u streamed, %u evicted, %llu vertex bytes, %llu index bytes",
               app->output_path,
               stats.uDrawnTilesLastFrame,
               stats.uLoadedTiles,
               stats.uManifestTiles,
               stats.uStreamedTilesLastFrame,
               stats.uEvictedTilesLastFrame,
               (unsigned long long)stats.ulVertexBytes,
               (unsigned long long)stats.ulIndexBytes);
        printf(", lods=");
        bool any_lod = false;
        for(uint32_t i = 0; i < 16u; i++)
        {
            if(stats.auDrawnTilesByLod[i] == 0)
                continue;
            printf("%s%u:%u", any_lod ? "," : "", i, stats.auDrawnTilesByLod[i]);
            any_lod = true;
        }
        if(!any_lod)
            printf("none");
        printf(")\n");
    }

    app->done = true;
    _ext_ioi->get_io()->bRunning = false;
}

PL_EXPORT void
pl_app_shutdown(AppData* app)
{
    if(!app)
        return;

    plDevice* device = (app->starter_initialized && _ext_starter) ? _ext_starter->get_device() : NULL;
    if(device && _ext_gfx)
        _ext_gfx->flush_device(device);

    if(app->view && _ext_planet)
        _ext_planet->cleanup_view(app->view);
    if(app->planet && _ext_planet)
        _ext_planet->cleanup_planet(app->planet);
    if(app->readback_buffer.uData && device && _ext_gfx)
        _ext_gfx->destroy_buffer(device, app->readback_buffer);

    if(app->planet_initialized && _ext_planet)
        _ext_planet->cleanup();
    if(app->shader_initialized && _ext_shader)
        _ext_shader->cleanup();
    if(app->starter_initialized && _ext_starter)
        _ext_starter->cleanup();
    if(app->window && _ext_windows)
        _ext_windows->destroy(app->window);

    PL_FREE(app);
}

PL_EXPORT void
pl_app_resize(plWindow* window, AppData* app)
{
    (void)window;
    (void)app;
    if(_ext_starter)
        _ext_starter->resize();
}

static void
_show_help(void)
{
    printf("Usage: dcapp-planet-snapshot <manifest.planet.json> --output FILE [options]\n\n");
    printf("Options:\n");
    printf("  --output FILE             Output PNG path\n");
    printf("  --width N                 Output width, default 1024\n");
    printf("  --height N                Output height, default 768\n");
    printf("  --gpu-cache-mb N          Resident GPU tile cache in MiB, default 512\n");
    printf("  --fov DEG                 Vertical FOV, default 45\n");
    printf("  --eye X Y Z               Cartesian camera position\n");
    printf("  --target X Y Z            Cartesian look-at target, default origin\n");
    printf("  --lod-threshold N         Screen-space LOD threshold in pixels, default 2\n");
    printf("  --vertex-shader FILE      Override vertex shader\n");
    printf("  --fragment-shader FILE    Override fragment shader\n");
    printf("  --wireframe               Render wireframe overlay mode\n");
    printf("  --show-levels             Add LOD debug color\n");
    printf("  --show-tiles              Add tile debug color\n");
    printf("  --flatten                 Render at manifest radius\n");
    printf("  --planet-snapshot-help   Show this help without invoking Pilotlight help\n");
}

static bool
_parse_args(int argc, char** argv, AppData* app)
{
    for(int i = 0; i < argc; i++)
    {
        if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "--planet-snapshot-help") == 0)
        {
            _show_help();
            return false;
        }
        else if(strcmp(argv[i], "--output") == 0 && i + 1 < argc)
        {
            app->output_path = argv[++i];
        }
        else if(strcmp(argv[i], "--width") == 0 && i + 1 < argc)
        {
            if(!_parse_uint(argv[++i], &app->width))
                return false;
        }
        else if(strcmp(argv[i], "--height") == 0 && i + 1 < argc)
        {
            if(!_parse_uint(argv[++i], &app->height))
                return false;
        }
        else if(strcmp(argv[i], "--gpu-cache-mb") == 0 && i + 1 < argc)
        {
            if(!_parse_uint(argv[++i], &app->gpu_cache_mb) || app->gpu_cache_mb == 0 || app->gpu_cache_mb > 4096u)
            {
                fprintf(stderr, "Error: --gpu-cache-mb must be in [1, 4096]\n");
                return false;
            }
        }
        else if(strcmp(argv[i], "--fov") == 0 && i + 1 < argc)
        {
            double value = 0.0;
            if(!_parse_double(argv[++i], &value))
                return false;
            app->fov = (float)value;
        }
        else if(strcmp(argv[i], "--eye") == 0 && i + 3 < argc)
        {
            if(!_parse_double(argv[++i], &app->eye.x) ||
               !_parse_double(argv[++i], &app->eye.y) ||
               !_parse_double(argv[++i], &app->eye.z))
            {
                return false;
            }
            app->has_eye = true;
        }
        else if(strcmp(argv[i], "--target") == 0 && i + 3 < argc)
        {
            if(!_parse_double(argv[++i], &app->target.x) ||
               !_parse_double(argv[++i], &app->target.y) ||
               !_parse_double(argv[++i], &app->target.z))
            {
                return false;
            }
            app->has_target = true;
        }
        else if(strcmp(argv[i], "--lod-threshold") == 0 && i + 1 < argc)
        {
            double value = 0.0;
            if(!_parse_double(argv[++i], &value) || value <= 0.0)
            {
                fprintf(stderr, "Error: --lod-threshold must be greater than zero\n");
                return false;
            }
            app->lod_threshold = (float)value;
        }
        else if(strcmp(argv[i], "--vertex-shader") == 0 && i + 1 < argc)
        {
            app->vertex_shader = argv[++i];
        }
        else if(strcmp(argv[i], "--fragment-shader") == 0 && i + 1 < argc)
        {
            app->fragment_shader = argv[++i];
        }
        else if(strcmp(argv[i], "--wireframe") == 0)
        {
            app->flags |= PL_PLANET_RENDER_FLAGS_WIREFRAME;
        }
        else if(strcmp(argv[i], "--show-levels") == 0)
        {
            app->flags |= PL_PLANET_RENDER_FLAGS_SHOW_LEVELS;
        }
        else if(strcmp(argv[i], "--show-tiles") == 0)
        {
            app->flags |= PL_PLANET_RENDER_FLAGS_SHOW_TILES;
        }
        else if(strcmp(argv[i], "--flatten") == 0)
        {
            app->flags |= PL_PLANET_RENDER_FLAGS_FLATTEN;
        }
        else if(argv[i][0] == '-')
        {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            return false;
        }
        else if(!app->manifest_path)
        {
            app->manifest_path = argv[i];
        }
        else
        {
            fprintf(stderr, "Error: unexpected argument: %s\n", argv[i]);
            return false;
        }
    }

    if(!app->manifest_path || !app->output_path)
    {
        fprintf(stderr, "Error: manifest path and --output are required\n");
        _show_help();
        return false;
    }
    if(app->width == 0 || app->height == 0)
    {
        fprintf(stderr, "Error: --width and --height must be greater than zero\n");
        return false;
    }
    if(app->fov <= 0.0f || app->fov >= 180.0f)
    {
        fprintf(stderr, "Error: --fov must be between 0 and 180 degrees\n");
        return false;
    }
    return true;
}

static bool
_parse_double(const char* text, double* out)
{
    if(!text || !out)
        return false;
    char* end = NULL;
    const double value = strtod(text, &end);
    if(end == text || (end && *end != '\0'))
        return false;
    *out = value;
    return true;
}

static bool
_parse_uint(const char* text, uint32_t* out)
{
    if(!text || !out)
        return false;
    char* end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if(end == text || (end && *end != '\0') || value > UINT32_MAX)
        return false;
    *out = (uint32_t)value;
    return true;
}
