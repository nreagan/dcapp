/*
------------------------------------------------------------------------------
pl_planet_ext.c

Canonical planet terrain contract:
- global cube-sphere tile addresses: face/lod/x/y
- sparse tile manifests with parent fallback
- DEM source catalog with footprint, priority, and native resolution
- source-choice helpers for multi-DEM chunk generation
------------------------------------------------------------------------------
*/

/*
Index of this file:
// [SECTION] includes
// [SECTION] globals
// [SECTION] internal helpers
// [SECTION] public api implementation
// [SECTION] extension loading
// [SECTION] unity build
*/

//-----------------------------------------------------------------------------
// [SECTION] includes
//-----------------------------------------------------------------------------

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pl.h"
#define PL_MATH_INCLUDE_FUNCTIONS
#include "pl_math.h"
#undef pl_vnsprintf
#include "pl_memory.h"

#include "pl_planet_ext.h"

#include "pl_graphics_ext.h"
#include "pl_shader_ext.h"
#include "pl_draw_ext.h"
#include "pl_camera_ext.h"

#include "pl_shader_interop_planet.h"

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

//-----------------------------------------------------------------------------
// [SECTION] globals
//-----------------------------------------------------------------------------

static const plMemoryI* gptMemory = NULL;
static const plGraphicsI* gptGfx = NULL;
static const plShaderI* gptShader = NULL;
static const plDrawI* gptDraw = NULL;

#define PL_ALLOC(x)      gptMemory->tracked_realloc(NULL, (x), __FILE__, __LINE__)
#define PL_REALLOC(x, y) gptMemory->tracked_realloc((x), (y), __FILE__, __LINE__)
#define PL_FREE(x)       gptMemory->tracked_realloc((x), 0, __FILE__, __LINE__)

#define PL_PLANET_PATH_MAX 512

typedef struct _plPlanetManifest
{
    plPlanetManifestInit tInit;

    plPlanetSourceRecord* atSources;
    uint32_t               uSourceCount;
    uint32_t               uSourceCapacity;

    plPlanetTileRecord* atTiles;
    uint32_t             uTileCount;
    uint32_t             uTileCapacity;

    char acManifestPath[PL_PLANET_PATH_MAX];
    char acBasePath[PL_PLANET_PATH_MAX];
    char acPayloadFormat[16];
    char acHeightMode[16];
    bool bPayloads;
    bool bFinalized;
} plPlanetManifest;

typedef struct _plPlanetGpuTile
{
    plPlanetTileRecord  tTile;
    plPlanetChunkHeader tHeader;
    uint32_t             uVertexStart;
    uint32_t             uIndexStart;
    uint32_t             uIndexCount;
    uint32_t             uCacheSlot;
    uint64_t             ulLastUsedFrame;
    bool                 bResident;
} plPlanetGpuTile;

typedef struct _plPlanet
{
    plPlanetManifest*       ptManifest;
    plPlanetRuntimeOptions  tRuntimeOptions;
    plPlanetRenderStats     tStats;

    plPlanetGpuTile* atGpuTiles;
    uint32_t          uGpuTileCount;
    int32_t*          aiTileIndexBySlot;
    uint32_t          uResidentCapacity;
    uint32_t          uResidentCount;
    uint64_t          ulFrameCounter;
    size_t            szSlotVertexBytes;
    size_t            szSlotIndexBytes;

    plBufferHandle tVertexBuffer;
    plBufferHandle tIndexBuffer;
} plPlanet;

typedef struct _plPlanetView
{
    plPlanet* ptPlanet;
    plPlanetViewRuntimeOptions tRuntimeOptions;

    plRenderPassHandle tRenderPass;
    plTextureHandle    tOutputTexture;
    plTextureHandle    tOutputTextureDepth;
    plBindGroupHandle  tOutputTextureHandle;

    uint32_t uOutputWidth;
    uint32_t uOutputHeight;

    plShaderHandle tShader;
    plShaderHandle tWireframeShader;
    const char*    pcVertexShader;
    const char*    pcFragmentShader;
} plPlanetView;

typedef struct _plPlanetContext
{
    plDevice*                ptDevice;
    plRenderPassLayoutHandle tRenderPassLayout;
    plDynamicDataBlock       tCurrentDynamicBufferBlock;
    uint32_t                 uStagingBufferSize;
    uint32_t                 uGpuCacheSize;
} plPlanetContext;

static plPlanetContext* gptCtx = NULL;

//-----------------------------------------------------------------------------
// [SECTION] internal helpers
//-----------------------------------------------------------------------------

static double
pl__planet_clamp(double x, double lo, double hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static double
pl__planet_absd(double x)
{
    return x < 0.0 ? -x : x;
}

static double
pl__planet_normalize_lon(double lon)
{
    double out = fmod(lon, 360.0);
    if(out < 0.0)
        out += 360.0;
    return out;
}

static bool
pl__planet_source_contains(const plPlanetSourceRecord* source, double latitude, double longitude)
{
    if(!source)
        return false;

    if(latitude < source->dMinLatitude || latitude > source->dMaxLatitude)
        return false;

    if((source->tFlags & PL_PLANET_SOURCE_FLAGS_GLOBAL_LONGITUDE) != 0)
        return true;

    const double lon = pl__planet_normalize_lon(longitude);
    const double minLon = pl__planet_normalize_lon(source->dMinLongitude);
    const double maxLon = pl__planet_normalize_lon(source->dMaxLongitude);

    if(minLon <= maxLon)
        return lon >= minLon && lon <= maxLon;

    // Antimeridian/seam-crossing footprint.
    return lon >= minLon || lon <= maxLon;
}

static bool
pl__planet_valid_address(plPlanetTileAddress address)
{
    if(address.tFace < 0 || address.tFace >= PL_PLANET_FACE_COUNT)
        return false;
    if(address.uLod >= 32)
        return false;

    const uint32_t dim = 1u << address.uLod;
    return address.uX < dim && address.uY < dim;
}

static int
pl__planet_compare_address(plPlanetTileAddress a, plPlanetTileAddress b)
{
    if(a.tFace != b.tFace)
        return a.tFace < b.tFace ? -1 : 1;
    if(a.uLod != b.uLod)
        return a.uLod < b.uLod ? -1 : 1;
    if(a.uY != b.uY)
        return a.uY < b.uY ? -1 : 1;
    if(a.uX != b.uX)
        return a.uX < b.uX ? -1 : 1;
    return 0;
}

static int
pl__planet_compare_tile_records(const void* a, const void* b)
{
    const plPlanetTileRecord* ta = (const plPlanetTileRecord*)a;
    const plPlanetTileRecord* tb = (const plPlanetTileRecord*)b;
    return pl__planet_compare_address(ta->tAddress, tb->tAddress);
}

static bool
pl__planet_reserve_sources(plPlanetManifest* manifest, uint32_t needed)
{
    if(needed <= manifest->uSourceCapacity)
        return true;

    uint32_t newCapacity = manifest->uSourceCapacity ? manifest->uSourceCapacity * 2u : 8u;
    while(newCapacity < needed)
        newCapacity *= 2u;

    plPlanetSourceRecord* newSources = (plPlanetSourceRecord*)PL_REALLOC(
        manifest->atSources,
        (size_t)newCapacity * sizeof(plPlanetSourceRecord));
    if(!newSources)
        return false;

    manifest->atSources = newSources;
    manifest->uSourceCapacity = newCapacity;
    return true;
}

static bool
pl__planet_reserve_tiles(plPlanetManifest* manifest, uint32_t needed)
{
    if(needed <= manifest->uTileCapacity)
        return true;

    uint32_t newCapacity = manifest->uTileCapacity ? manifest->uTileCapacity * 2u : 64u;
    while(newCapacity < needed)
        newCapacity *= 2u;

    plPlanetTileRecord* newTiles = (plPlanetTileRecord*)PL_REALLOC(
        manifest->atTiles,
        (size_t)newCapacity * sizeof(plPlanetTileRecord));
    if(!newTiles)
        return false;

    manifest->atTiles = newTiles;
    manifest->uTileCapacity = newCapacity;
    return true;
}

static plVec3d
pl__planet_norm_vec3d(plVec3d v)
{
    const double len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if(len <= DBL_EPSILON)
        return (plVec3d){0.0, 0.0, 1.0};
    return (plVec3d){v.x / len, v.y / len, v.z / len};
}

static bool
pl__planet_is_absolute_path(const char* path)
{
    if(!path || path[0] == '\0')
        return false;
    if(path[0] == '/')
        return true;
#ifdef _WIN32
    if((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
        return path[1] == ':';
    if(path[0] == '\\' && path[1] == '\\')
        return true;
#endif
    return false;
}

static void
pl__planet_copy_string(char* dst, uint32_t dstSize, const char* src)
{
    if(!dst || dstSize == 0)
        return;
    dst[0] = '\0';
    if(!src)
        return;
    snprintf(dst, dstSize, "%s", src);
}

static void
pl__planet_dirname(const char* path, char* outDir, uint32_t outDirSize)
{
    if(!outDir || outDirSize == 0)
        return;
    outDir[0] = '\0';
    if(!path || path[0] == '\0')
    {
        snprintf(outDir, outDirSize, ".");
        return;
    }

    const char* slash = strrchr(path, '/');
#ifdef _WIN32
    const char* backslash = strrchr(path, '\\');
    if(!slash || (backslash && backslash > slash))
        slash = backslash;
#endif
    if(!slash)
    {
        snprintf(outDir, outDirSize, ".");
        return;
    }

    const size_t len = (size_t)(slash - path);
    if(len == 0)
    {
        snprintf(outDir, outDirSize, "/");
        return;
    }

    const size_t copyLen = len < (size_t)(outDirSize - 1u) ? len : (size_t)(outDirSize - 1u);
    memcpy(outDir, path, copyLen);
    outDir[copyLen] = '\0';
}

static bool
pl__planet_read_file_text(const char* path, char** outText)
{
    if(outText)
        *outText = NULL;
    if(!path || !outText)
        return false;

    FILE* file = fopen(path, "rb");
    if(!file)
        return false;

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if(size < 0)
    {
        fclose(file);
        return false;
    }

    char* text = (char*)PL_ALLOC((size_t)size + 1u);
    if(!text)
    {
        fclose(file);
        return false;
    }

    const size_t readSize = fread(text, 1, (size_t)size, file);
    fclose(file);
    if(readSize != (size_t)size)
    {
        PL_FREE(text);
        return false;
    }

    text[size] = '\0';
    *outText = text;
    return true;
}

static bool
pl__planet_chunk_header_valid(const plPlanetChunkHeader* header)
{
    if(!header)
        return false;
    if(header->uMagic != PL_PLANET_CHUNK_MAGIC)
        return false;
    if(header->uVersionMajor != PL_PLANET_CHUNK_VERSION_MAJOR)
        return false;
    if(header->uHeaderSize < sizeof(plPlanetChunkHeader))
        return false;
    if(header->uVertexSize != sizeof(plPlanetVertex))
        return false;
    if(header->uVertexCount == 0 || header->uIndexCount == 0)
        return false;
    if(header->ulVertexDataOffset < header->uHeaderSize)
        return false;
    if(header->ulIndexDataOffset <= header->ulVertexDataOffset)
        return false;
    return true;
}

//-----------------------------------------------------------------------------
// [SECTION] public api implementation
//-----------------------------------------------------------------------------

static void
pl_planet_initialize(plPlanetExtInit init)
{
    if(!gptCtx)
        return;

    gptCtx->ptDevice = init.ptDevice;
    gptCtx->uStagingBufferSize = init.uStagingBufferSize ? init.uStagingBufferSize : 67108864u;
    gptCtx->uGpuCacheSize = init.uGpuCacheSize ? init.uGpuCacheSize : 536870912u;

    if(!gptGfx || !gptCtx->ptDevice)
        return;

    const plRenderPassLayoutDesc renderPassLayoutDesc = {
        .atRenderTargets = {
            { .tFormat = PL_FORMAT_D32_FLOAT_S8_UINT, .bDepth = true },
            { .tFormat = PL_FORMAT_R8G8B8A8_UNORM },
        },
        .atSubpasses = {
            {
                .uRenderTargetCount = 2,
                .auRenderTargets = {0, 1},
            }
        },
        .atSubpassDependencies = {
            {
                .uSourceSubpass         = UINT32_MAX,
                .uDestinationSubpass    = 0,
                .tSourceStageMask       = PL_PIPELINE_STAGE_FRAGMENT_SHADER | PL_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT | PL_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS | PL_PIPELINE_STAGE_LATE_FRAGMENT_TESTS | PL_PIPELINE_STAGE_COMPUTE_SHADER,
                .tDestinationStageMask  = PL_PIPELINE_STAGE_FRAGMENT_SHADER | PL_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT | PL_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS | PL_PIPELINE_STAGE_LATE_FRAGMENT_TESTS,
                .tSourceAccessMask      = PL_ACCESS_SHADER_READ | PL_ACCESS_COLOR_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ,
                .tDestinationAccessMask = PL_ACCESS_SHADER_READ | PL_ACCESS_COLOR_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ,
            },
            {
                .uSourceSubpass         = 0,
                .uDestinationSubpass    = UINT32_MAX,
                .tSourceStageMask       = PL_PIPELINE_STAGE_FRAGMENT_SHADER | PL_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT | PL_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS | PL_PIPELINE_STAGE_LATE_FRAGMENT_TESTS,
                .tDestinationStageMask  = PL_PIPELINE_STAGE_FRAGMENT_SHADER | PL_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT | PL_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS | PL_PIPELINE_STAGE_LATE_FRAGMENT_TESTS | PL_PIPELINE_STAGE_COMPUTE_SHADER,
                .tSourceAccessMask      = PL_ACCESS_SHADER_READ | PL_ACCESS_COLOR_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ,
                .tDestinationAccessMask = PL_ACCESS_SHADER_READ | PL_ACCESS_COLOR_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE | PL_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ,
            }
        }
    };
    gptCtx->tRenderPassLayout = gptGfx->create_render_pass_layout(gptCtx->ptDevice, &renderPassLayoutDesc);
}

static void
pl_planet_cleanup(void)
{
    if(!gptCtx || !gptGfx || !gptCtx->ptDevice)
        return;

    if(gptCtx->tRenderPassLayout.uData)
        gptGfx->destroy_render_pass_layout(gptCtx->ptDevice, gptCtx->tRenderPassLayout);
    gptCtx->tRenderPassLayout = (plRenderPassLayoutHandle){0};
    gptCtx->ptDevice = NULL;
}

static plPlanetManifest*
pl_planet_create_manifest(plPlanetManifestInit init)
{
    if(init.dRadius <= 0.0)
        return NULL;
    if(init.uTileSize == 0)
        init.uTileSize = 257;
    if(init.tTilingMode != PL_PLANET_TILING_CUBE_SPHERE)
        return NULL;

    plPlanetManifest* manifest = (plPlanetManifest*)PL_ALLOC(sizeof(plPlanetManifest));
    if(!manifest)
        return NULL;

    memset(manifest, 0, sizeof(*manifest));
    manifest->tInit = init;
    return manifest;
}

static void
pl_planet_cleanup_manifest(plPlanetManifest* manifest)
{
    if(!manifest)
        return;

    if(manifest->atSources)
        PL_FREE(manifest->atSources);
    if(manifest->atTiles)
        PL_FREE(manifest->atTiles);
    PL_FREE(manifest);
}

static uint32_t
pl_planet_add_source(plPlanetManifest* manifest, plPlanetSourceRecord source)
{
    if(!manifest)
        return UINT32_MAX;
    if(!pl__planet_reserve_sources(manifest, manifest->uSourceCount + 1u))
        return UINT32_MAX;

    source.dMinLongitude = pl__planet_normalize_lon(source.dMinLongitude);
    source.dMaxLongitude = pl__planet_normalize_lon(source.dMaxLongitude);
    if(source.dMinLatitude > source.dMaxLatitude)
    {
        const double tmp = source.dMinLatitude;
        source.dMinLatitude = source.dMaxLatitude;
        source.dMaxLatitude = tmp;
    }

    const uint32_t index = manifest->uSourceCount++;
    manifest->atSources[index] = source;
    manifest->bFinalized = false;
    return index;
}

static bool
pl_planet_add_tile(plPlanetManifest* manifest, plPlanetTileRecord tile)
{
    if(!manifest || !pl__planet_valid_address(tile.tAddress))
        return false;
    if(tile.uSourceIndex != UINT32_MAX && tile.uSourceIndex >= manifest->uSourceCount)
        return false;
    if(tile.tAddress.uLod > manifest->tInit.uMaxLod)
        return false;
    if(!pl__planet_reserve_tiles(manifest, manifest->uTileCount + 1u))
        return false;

    tile.tFlags |= PL_PLANET_TILE_FLAGS_AVAILABLE;
    manifest->atTiles[manifest->uTileCount++] = tile;
    manifest->bFinalized = false;
    return true;
}

static void
pl_planet_finalize_manifest(plPlanetManifest* manifest)
{
    if(!manifest || manifest->uTileCount == 0)
    {
        if(manifest)
            manifest->bFinalized = true;
        return;
    }

    qsort(manifest->atTiles, manifest->uTileCount, sizeof(plPlanetTileRecord), pl__planet_compare_tile_records);
    manifest->bFinalized = true;
}

static bool
pl_planet_resolve_tile_path(const plPlanetManifest* manifest, const plPlanetTileRecord* tile, char* outPath, uint32_t outPathSize);

static bool
pl_planet_read_chunk_header(const char* path, plPlanetChunkHeader* outHeader);

static plPlanetManifest*
pl_planet_load_manifest_json(const char* path, bool validatePayloads)
{
    char* jsonText = NULL;
    if(!pl__planet_read_file_text(path, &jsonText))
        return NULL;

    plJsonObject* root = NULL;
    if(!pl_load_json(jsonText, &root))
    {
        PL_FREE(jsonText);
        return NULL;
    }

    char schema[32] = {0};
    pl_json_string_member(root, "schema", schema, sizeof(schema));
    if(strcmp(schema, "planet") != 0)
    {
        pl_unload_json(&root);
        PL_FREE(jsonText);
        return NULL;
    }

    plPlanetManifestInit init = {
        .dRadius = pl_json_double_member(root, "radius", 0.0),
        .uTileSize = (uint32_t)pl_json_int_member(root, "tile_size", 0),
        .uMaxLod = (uint8_t)pl_json_int_member(root, "max_lod", 0),
        .tTilingMode = PL_PLANET_TILING_CUBE_SPHERE,
        .tFlags = PL_PLANET_MANIFEST_FLAGS_NONE
    };

    plPlanetManifest* manifest = pl_planet_create_manifest(init);
    if(!manifest)
    {
        pl_unload_json(&root);
        PL_FREE(jsonText);
        return NULL;
    }

    pl__planet_copy_string(manifest->acManifestPath, sizeof(manifest->acManifestPath), path);
    pl__planet_dirname(path, manifest->acBasePath, sizeof(manifest->acBasePath));
    manifest->bPayloads = pl_json_bool_member(root, "payloads", false);
    pl_json_string_member(root, "payload_format", manifest->acPayloadFormat, sizeof(manifest->acPayloadFormat));
    pl_json_string_member(root, "height_mode", manifest->acHeightMode, sizeof(manifest->acHeightMode));

    uint32_t sourceCount = 0;
    plJsonObject* sources = pl_json_array_member(root, "sources", &sourceCount);
    if(!sources && sourceCount == 0)
    {
        pl_planet_cleanup_manifest(manifest);
        pl_unload_json(&root);
        PL_FREE(jsonText);
        return NULL;
    }

    for(uint32_t i = 0; i < sourceCount; i++)
    {
        plJsonObject* obj = pl_json_member_by_index(sources, i);
        if(!obj)
            continue;

        plPlanetSourceRecord source = {0};
        pl_json_string_member(obj, "name", source.acName, sizeof(source.acName));
        pl_json_string_member(obj, "path", source.acPath, sizeof(source.acPath));
        source.tFlags = pl_json_int_member(obj, "flags", 0);
        source.uPriority = pl_json_int_member(obj, "priority", 0);
        source.dNativeMetersPerPixel = pl_json_double_member(obj, "native_meters_per_pixel", 0.0);
        source.dMinHeight = pl_json_double_member(obj, "min_height", 0.0);
        source.dMaxHeight = pl_json_double_member(obj, "max_height", 0.0);

        plJsonObject* footprint = pl_json_member(obj, "footprint");
        if(footprint)
        {
            source.dMinLatitude = pl_json_double_member(footprint, "min_latitude", 0.0);
            source.dMaxLatitude = pl_json_double_member(footprint, "max_latitude", 0.0);
            source.dMinLongitude = pl_json_double_member(footprint, "min_longitude", 0.0);
            source.dMaxLongitude = pl_json_double_member(footprint, "max_longitude", 0.0);
        }

        if(pl_planet_add_source(manifest, source) == UINT32_MAX)
        {
            pl_planet_cleanup_manifest(manifest);
            pl_unload_json(&root);
            PL_FREE(jsonText);
            return NULL;
        }
    }

    uint32_t tileCount = 0;
    plJsonObject* tiles = pl_json_array_member(root, "tiles", &tileCount);
    if(!tiles && tileCount == 0)
    {
        pl_planet_cleanup_manifest(manifest);
        pl_unload_json(&root);
        PL_FREE(jsonText);
        return NULL;
    }

    for(uint32_t i = 0; i < tileCount; i++)
    {
        plJsonObject* obj = pl_json_member_by_index(tiles, i);
        if(!obj)
            continue;

        plPlanetTileRecord tile = {0};
        tile.tAddress.tFace = pl_json_int_member(obj, "face", -1);
        tile.tAddress.uLod = (uint8_t)pl_json_int_member(obj, "lod", 0);
        tile.tAddress.uX = (uint32_t)pl_json_int_member(obj, "x", 0);
        tile.tAddress.uY = (uint32_t)pl_json_int_member(obj, "y", 0);
        tile.uSourceIndex = (uint32_t)pl_json_int_member(obj, "source", 0);
        tile.dGeometricError = pl_json_double_member(obj, "geometric_error", 0.0);
        tile.dMinHeight = pl_json_double_member(obj, "min_height", 0.0);
        tile.dMaxHeight = pl_json_double_member(obj, "max_height", 0.0);
        pl_json_string_member(obj, "file", tile.acChunkFile, sizeof(tile.acChunkFile));
        tile.ulByteOffset = (uint64_t)pl_json_uint_member(obj, "byte_offset", 0);
        tile.ulByteSize = (uint64_t)pl_json_uint_member(obj, "byte_size", 0);

        if(!pl_planet_add_tile(manifest, tile))
        {
            pl_planet_cleanup_manifest(manifest);
            pl_unload_json(&root);
            PL_FREE(jsonText);
            return NULL;
        }
    }

    pl_planet_finalize_manifest(manifest);

    if(validatePayloads && manifest->bPayloads)
    {
        char resolvedPath[PL_PLANET_PATH_MAX];
        for(uint32_t i = 0; i < manifest->uTileCount; i++)
        {
            const plPlanetTileRecord* tile = &manifest->atTiles[i];
            if(tile->ulByteSize == 0)
            {
                pl_planet_cleanup_manifest(manifest);
                pl_unload_json(&root);
                PL_FREE(jsonText);
                return NULL;
            }

            if(!pl_planet_resolve_tile_path(manifest, tile, resolvedPath, sizeof(resolvedPath)))
            {
                pl_planet_cleanup_manifest(manifest);
                pl_unload_json(&root);
                PL_FREE(jsonText);
                return NULL;
            }

            plPlanetChunkHeader header = {0};
            if(!pl_planet_read_chunk_header(resolvedPath, &header))
            {
                pl_planet_cleanup_manifest(manifest);
                pl_unload_json(&root);
                PL_FREE(jsonText);
                return NULL;
            }

            if(header.iFace != tile->tAddress.tFace ||
               header.uLod != tile->tAddress.uLod ||
               header.uX != tile->tAddress.uX ||
               header.uY != tile->tAddress.uY ||
               header.uSourceIndex != tile->uSourceIndex ||
               header.uTileSize != manifest->tInit.uTileSize)
            {
                pl_planet_cleanup_manifest(manifest);
                pl_unload_json(&root);
                PL_FREE(jsonText);
                return NULL;
            }
        }
    }

    pl_unload_json(&root);
    PL_FREE(jsonText);
    return manifest;
}

static const plPlanetManifestInit*
pl_planet_get_manifest_init(const plPlanetManifest* manifest)
{
    return manifest ? &manifest->tInit : NULL;
}

static uint32_t
pl_planet_get_source_count(const plPlanetManifest* manifest)
{
    return manifest ? manifest->uSourceCount : 0u;
}

static const plPlanetSourceRecord*
pl_planet_get_source(const plPlanetManifest* manifest, uint32_t sourceIndex)
{
    if(!manifest || sourceIndex >= manifest->uSourceCount)
        return NULL;
    return &manifest->atSources[sourceIndex];
}

static uint32_t
pl_planet_get_tile_count(const plPlanetManifest* manifest)
{
    return manifest ? manifest->uTileCount : 0u;
}

static const plPlanetTileRecord*
pl_planet_get_tile(const plPlanetManifest* manifest, uint32_t tileIndex)
{
    if(!manifest || tileIndex >= manifest->uTileCount)
        return NULL;
    return &manifest->atTiles[tileIndex];
}

static bool
pl_planet_manifest_has_payloads(const plPlanetManifest* manifest)
{
    return manifest ? manifest->bPayloads : false;
}

static const char*
pl_planet_get_manifest_path(const plPlanetManifest* manifest)
{
    return manifest ? manifest->acManifestPath : NULL;
}

static const char*
pl_planet_get_manifest_base_path(const plPlanetManifest* manifest)
{
    return manifest ? manifest->acBasePath : NULL;
}

static plPlanetTileAddress
pl_planet_make_address(plPlanetFace face, uint8_t lod, uint32_t x, uint32_t y)
{
    return (plPlanetTileAddress){
        .tFace = face,
        .uLod = lod,
        .uX = x,
        .uY = y
    };
}

static plPlanetTileAddress
pl_planet_parent_address(plPlanetTileAddress address)
{
    if(address.uLod == 0)
        return address;

    return (plPlanetTileAddress){
        .tFace = address.tFace,
        .uLod = (uint8_t)(address.uLod - 1u),
        .uX = address.uX >> 1u,
        .uY = address.uY >> 1u
    };
}

static bool
pl_planet_find_tile(const plPlanetManifest* manifest, plPlanetTileAddress address, const plPlanetTileRecord** outTile)
{
    if(outTile)
        *outTile = NULL;
    if(!manifest || !manifest->atTiles || manifest->uTileCount == 0 || !pl__planet_valid_address(address))
        return false;

    if(!manifest->bFinalized)
    {
        for(uint32_t i = 0; i < manifest->uTileCount; i++)
        {
            if(pl__planet_compare_address(manifest->atTiles[i].tAddress, address) == 0)
            {
                if(outTile)
                    *outTile = &manifest->atTiles[i];
                return true;
            }
        }
        return false;
    }

    uint32_t lo = 0;
    uint32_t hi = manifest->uTileCount;
    while(lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const int cmp = pl__planet_compare_address(manifest->atTiles[mid].tAddress, address);
        if(cmp == 0)
        {
            if(outTile)
                *outTile = &manifest->atTiles[mid];
            return true;
        }
        if(cmp < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }

    return false;
}

static bool
pl_planet_select_tile(const plPlanetManifest* manifest, plPlanetTileAddress desired, plPlanetTileSelection* outSelection)
{
    if(outSelection)
        memset(outSelection, 0, sizeof(*outSelection));
    if(!manifest || !outSelection || !pl__planet_valid_address(desired))
        return false;

    plPlanetTileAddress current = desired;
    uint8_t fallback = 0;
    const plPlanetTileRecord* tile = NULL;

    for(;;)
    {
        if(pl_planet_find_tile(manifest, current, &tile))
        {
            outSelection->ptTile = tile;
            outSelection->tRequested = desired;
            outSelection->tSelected = current;
            outSelection->uFallbackLevels = fallback;
            outSelection->bExact = fallback == 0;
            return true;
        }

        if(current.uLod == 0)
            return false;

        current = pl_planet_parent_address(current);
        fallback++;
    }
}

static bool
pl_planet_resolve_tile_path(const plPlanetManifest* manifest, const plPlanetTileRecord* tile, char* outPath, uint32_t outPathSize)
{
    if(!manifest || !tile || !outPath || outPathSize == 0 || tile->acChunkFile[0] == '\0')
        return false;

    if(pl__planet_is_absolute_path(tile->acChunkFile) || manifest->acBasePath[0] == '\0')
    {
        snprintf(outPath, outPathSize, "%s", tile->acChunkFile);
        return outPath[0] != '\0';
    }

    const char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif

    const size_t baseLen = strlen(manifest->acBasePath);
    if(baseLen > 0 && (manifest->acBasePath[baseLen - 1u] == '/' || manifest->acBasePath[baseLen - 1u] == '\\'))
        snprintf(outPath, outPathSize, "%s%s", manifest->acBasePath, tile->acChunkFile);
    else
        snprintf(outPath, outPathSize, "%s%c%s", manifest->acBasePath, sep, tile->acChunkFile);
    return outPath[0] != '\0';
}

static bool
pl_planet_read_chunk_header(const char* path, plPlanetChunkHeader* outHeader)
{
    if(outHeader)
        memset(outHeader, 0, sizeof(*outHeader));
    if(!path || !outHeader)
        return false;

    FILE* file = fopen(path, "rb");
    if(!file)
        return false;

    plPlanetChunkHeader header = {0};
    const size_t readSize = fread(&header, 1, sizeof(header), file);
    fclose(file);
    if(readSize != sizeof(header))
        return false;
    if(!pl__planet_chunk_header_valid(&header))
        return false;

    *outHeader = header;
    return true;
}

static bool
pl_planet_read_chunk_payload(const char* path, const plPlanetChunkHeader* header, plPlanetVertex* outVertices, uint32_t* outIndices)
{
    if(!path || !outVertices || !outIndices)
        return false;

    plPlanetChunkHeader localHeader = {0};
    const plPlanetChunkHeader* useHeader = header;
    if(!useHeader)
    {
        if(!pl_planet_read_chunk_header(path, &localHeader))
            return false;
        useHeader = &localHeader;
    }

    if(!pl__planet_chunk_header_valid(useHeader))
        return false;

    FILE* file = fopen(path, "rb");
    if(!file)
        return false;

    if(fseek(file, (long)useHeader->ulVertexDataOffset, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }

    const size_t vertexBytes = (size_t)useHeader->uVertexCount * sizeof(plPlanetVertex);
    if(fread(outVertices, 1, vertexBytes, file) != vertexBytes)
    {
        fclose(file);
        return false;
    }

    if(fseek(file, (long)useHeader->ulIndexDataOffset, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }

    const size_t indexBytes = (size_t)useHeader->uIndexCount * sizeof(uint32_t);
    if(fread(outIndices, 1, indexBytes, file) != indexBytes)
    {
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

static void
pl__planet_split_double(double value, float* outHigh, float* outLow)
{
    *outHigh = (float)value;
    *outLow = (float)(value - (double)*outHigh);
}

static int
pl__planet_to_shader_flags(plPlanetRenderFlags flags)
{
    int shaderFlags = PL_TERRAIN_SHADER_FLAGS_NONE;
    if(flags & PL_PLANET_RENDER_FLAGS_WIREFRAME)
        shaderFlags |= PL_TERRAIN_SHADER_FLAGS_WIREFRAME;
    if(flags & PL_PLANET_RENDER_FLAGS_SHOW_LEVELS)
        shaderFlags |= PL_TERRAIN_SHADER_FLAGS_SHOW_LEVELS;
    if(flags & PL_PLANET_RENDER_FLAGS_SHOW_TILES)
        shaderFlags |= PL_TERRAIN_SHADER_FLAGS_SHOW_CHUNKS;
    if(flags & PL_PLANET_RENDER_FLAGS_FLATTEN)
        shaderFlags |= PL_TERRAIN_SHADER_FLAGS_FLATTEN;
    return shaderFlags;
}

static plTextureHandle
pl__planet_create_texture(plCommandBuffer* cmdBuffer, const plTextureDesc* desc, const char* name)
{
    (void)cmdBuffer;
    (void)name;

    if(!gptCtx || !gptGfx || !gptCtx->ptDevice || !desc)
        return (plTextureHandle){0};

    plTexture* texture = NULL;
    const plTextureHandle handle = gptGfx->create_texture(gptCtx->ptDevice, desc, &texture);
    if(!handle.uData || !texture)
        return (plTextureHandle){0};

    const plDeviceMemoryAllocation allocation = gptGfx->allocate_memory(
        gptCtx->ptDevice,
        texture->tMemoryRequirements.ulSize,
        PL_MEMORY_FLAGS_DEVICE_LOCAL,
        texture->tMemoryRequirements.uMemoryTypeBits,
        name ? name : "planet texture memory");

    gptGfx->bind_texture_to_memory(gptCtx->ptDevice, handle, &allocation);
    return handle;
}

static bool
pl__planet_address_matches_header(const plPlanetTileRecord* tile, const plPlanetChunkHeader* header)
{
    if(!tile || !header)
        return false;

    return header->iFace == tile->tAddress.tFace &&
           header->uLod == tile->tAddress.uLod &&
           header->uX == tile->tAddress.uX &&
           header->uY == tile->tAddress.uY &&
           header->uSourceIndex == tile->uSourceIndex;
}

static plPlanet*
pl_planet_create_planet(plCommandBuffer* cmdBuffer, plPlanetInit init)
{
    (void)cmdBuffer;
    if(!gptCtx || !gptGfx || !gptCtx->ptDevice || !cmdBuffer)
        return NULL;

    plPlanetManifest* manifest = init.ptManifest;
    if(!manifest && init.pcManifestPath)
        manifest = pl_planet_load_manifest_json(init.pcManifestPath, init.bValidatePayloads);
    if(!manifest || !manifest->bPayloads || manifest->uTileCount == 0)
    {
        if(manifest && manifest != init.ptManifest)
            pl_planet_cleanup_manifest(manifest);
        return NULL;
    }

    plPlanet* planet = (plPlanet*)PL_ALLOC(sizeof(plPlanet));
    if(!planet)
    {
        pl_planet_cleanup_manifest(manifest);
        return NULL;
    }
    memset(planet, 0, sizeof(*planet));
    planet->ptManifest = manifest;
    planet->tRuntimeOptions.tLightDirection = (plVec3){-1.0f, -1.0f, -1.0f};

    planet->atGpuTiles = (plPlanetGpuTile*)PL_ALLOC((size_t)manifest->uTileCount * sizeof(plPlanetGpuTile));
    if(!planet->atGpuTiles)
    {
        pl_planet_cleanup_manifest(manifest);
        PL_FREE(planet);
        return NULL;
    }
    memset(planet->atGpuTiles, 0, (size_t)manifest->uTileCount * sizeof(plPlanetGpuTile));

    uint64_t totalVertexBytes = 0;
    uint64_t totalIndexBytes = 0;
    size_t maxVertexBytes = 0;
    size_t maxIndexBytes = 0;
    char resolvedPath[PL_PLANET_PATH_MAX] = {0};

    for(uint32_t i = 0; i < manifest->uTileCount; i++)
    {
        const plPlanetTileRecord* tile = &manifest->atTiles[i];
        if(tile->ulByteOffset != 0)
            goto fail;
        if(!pl_planet_resolve_tile_path(manifest, tile, resolvedPath, sizeof(resolvedPath)))
            goto fail;

        plPlanetChunkHeader header = {0};
        if(!pl_planet_read_chunk_header(resolvedPath, &header))
            goto fail;
        if(!pl__planet_address_matches_header(tile, &header))
            goto fail;
        if(header.uTileSize != manifest->tInit.uTileSize)
            goto fail;

        const uint64_t vertexBytes = (uint64_t)header.uVertexCount * sizeof(plPlanetVertex);
        const uint64_t indexBytes = (uint64_t)header.uIndexCount * sizeof(uint32_t);
        if(vertexBytes > (uint64_t)SIZE_MAX || indexBytes > (uint64_t)SIZE_MAX)
            goto fail;

        plPlanetGpuTile* gpuTile = &planet->atGpuTiles[planet->uGpuTileCount++];
        gpuTile->tTile = *tile;
        gpuTile->tHeader = header;
        gpuTile->uIndexCount = header.uIndexCount;
        gpuTile->uCacheSlot = UINT32_MAX;

        if((size_t)vertexBytes > maxVertexBytes)
            maxVertexBytes = (size_t)vertexBytes;
        if((size_t)indexBytes > maxIndexBytes)
            maxIndexBytes = (size_t)indexBytes;
        totalVertexBytes += vertexBytes;
        totalIndexBytes += indexBytes;
    }

    const uint64_t totalUploadBytes = totalVertexBytes + totalIndexBytes;

    if(maxVertexBytes == 0 || maxIndexBytes == 0 || totalUploadBytes < totalVertexBytes)
        goto fail;

    const uint64_t maxTileBytes = (uint64_t)maxVertexBytes + (uint64_t)maxIndexBytes;
    if(maxTileBytes == 0 || maxTileBytes > (uint64_t)SIZE_MAX)
        goto fail;

    const uint64_t cacheBudget = gptCtx->uGpuCacheSize ? gptCtx->uGpuCacheSize : 536870912ull;
    uint32_t residentCapacity = planet->uGpuTileCount;
    if(totalUploadBytes > cacheBudget)
    {
        residentCapacity = (uint32_t)(cacheBudget / maxTileBytes);
        if(residentCapacity < 6u)
            residentCapacity = planet->uGpuTileCount < 6u ? planet->uGpuTileCount : 6u;
    }
    if(residentCapacity == 0 || residentCapacity > planet->uGpuTileCount)
        residentCapacity = planet->uGpuTileCount;

    planet->uResidentCapacity = residentCapacity;
    planet->szSlotVertexBytes = maxVertexBytes;
    planet->szSlotIndexBytes = maxIndexBytes;
    planet->aiTileIndexBySlot = (int32_t*)PL_ALLOC((size_t)residentCapacity * sizeof(int32_t));
    if(!planet->aiTileIndexBySlot)
        goto fail;
    for(uint32_t i = 0; i < residentCapacity; i++)
        planet->aiTileIndexBySlot[i] = -1;

    const uint64_t cacheVertexBytes = (uint64_t)residentCapacity * (uint64_t)maxVertexBytes;
    const uint64_t cacheIndexBytes = (uint64_t)residentCapacity * (uint64_t)maxIndexBytes;
    if(cacheVertexBytes > (uint64_t)SIZE_MAX || cacheIndexBytes > (uint64_t)SIZE_MAX)
        goto fail;

    {
        const plBufferDesc vertexBufferDesc = {
            .tUsage = PL_BUFFER_USAGE_VERTEX | PL_BUFFER_USAGE_TRANSFER_DESTINATION,
            .szByteSize = (size_t)cacheVertexBytes,
            .pcDebugName = "planet resident vertex cache"
        };
        plBuffer* vertexBuffer = NULL;
        planet->tVertexBuffer = gptGfx->create_buffer(gptCtx->ptDevice, &vertexBufferDesc, &vertexBuffer);
        if(!planet->tVertexBuffer.uData || !vertexBuffer)
            goto fail;

        const plDeviceMemoryAllocation vertexAllocation = gptGfx->allocate_memory(
            gptCtx->ptDevice,
            vertexBuffer->tMemoryRequirements.ulSize,
            PL_MEMORY_FLAGS_DEVICE_LOCAL,
            vertexBuffer->tMemoryRequirements.uMemoryTypeBits,
            "planet vertex buffer memory");
        gptGfx->bind_buffer_to_memory(gptCtx->ptDevice, planet->tVertexBuffer, &vertexAllocation);

        const plBufferDesc indexBufferDesc = {
            .tUsage = PL_BUFFER_USAGE_INDEX | PL_BUFFER_USAGE_TRANSFER_DESTINATION,
            .szByteSize = (size_t)cacheIndexBytes,
            .pcDebugName = "planet resident index cache"
        };
        plBuffer* indexBuffer = NULL;
        planet->tIndexBuffer = gptGfx->create_buffer(gptCtx->ptDevice, &indexBufferDesc, &indexBuffer);
        if(!planet->tIndexBuffer.uData || !indexBuffer)
            goto fail;

        const plDeviceMemoryAllocation indexAllocation = gptGfx->allocate_memory(
            gptCtx->ptDevice,
            indexBuffer->tMemoryRequirements.ulSize,
            PL_MEMORY_FLAGS_DEVICE_LOCAL,
            indexBuffer->tMemoryRequirements.uMemoryTypeBits,
            "planet index buffer memory");
        gptGfx->bind_buffer_to_memory(gptCtx->ptDevice, planet->tIndexBuffer, &indexAllocation);
    }

    planet->tStats.uManifestTiles = planet->uGpuTileCount;
    planet->tStats.uLoadedTiles = 0;
    planet->tStats.ulVertexBytes = cacheVertexBytes;
    planet->tStats.ulIndexBytes = cacheIndexBytes;
    return planet;

fail:
    if(planet)
    {
        if(planet->tVertexBuffer.uData)
            gptGfx->destroy_buffer(gptCtx->ptDevice, planet->tVertexBuffer);
        if(planet->tIndexBuffer.uData)
            gptGfx->destroy_buffer(gptCtx->ptDevice, planet->tIndexBuffer);
        if(planet->aiTileIndexBySlot)
            PL_FREE(planet->aiTileIndexBySlot);
        if(planet->atGpuTiles)
            PL_FREE(planet->atGpuTiles);
        if(planet->ptManifest)
            pl_planet_cleanup_manifest(planet->ptManifest);
        PL_FREE(planet);
    }
    return NULL;
}

static void
pl_planet_cleanup_planet(plPlanet* planet)
{
    if(!planet)
        return;

    if(gptCtx && gptGfx && gptCtx->ptDevice)
    {
        if(planet->tVertexBuffer.uData)
            gptGfx->destroy_buffer(gptCtx->ptDevice, planet->tVertexBuffer);
        if(planet->tIndexBuffer.uData)
            gptGfx->destroy_buffer(gptCtx->ptDevice, planet->tIndexBuffer);
    }

    if(planet->ptManifest)
        pl_planet_cleanup_manifest(planet->ptManifest);
    if(planet->aiTileIndexBySlot)
        PL_FREE(planet->aiTileIndexBySlot);
    if(planet->atGpuTiles)
        PL_FREE(planet->atGpuTiles);
    PL_FREE(planet);
}

static void
pl_planet_load_shaders(plPlanetView* view)
{
    if(!view || !gptCtx || !gptGfx || !gptShader || !gptCtx->ptDevice || !gptCtx->tRenderPassLayout.uData)
        return;

    if(gptGfx->is_shader_valid(gptCtx->ptDevice, view->tShader))
        gptGfx->queue_shader_for_deletion(gptCtx->ptDevice, view->tShader);
    if(gptGfx->is_shader_valid(gptCtx->ptDevice, view->tWireframeShader))
        gptGfx->queue_shader_for_deletion(gptCtx->ptDevice, view->tWireframeShader);

    const plShaderDesc shaderDescBase = {
        .tVertexShader = gptShader->load_glsl(view->pcVertexShader, "main", NULL, NULL),
        .tFragmentShader = gptShader->load_glsl(view->pcFragmentShader, "main", NULL, NULL),
        .tGraphicsState = {
            .ulDepthWriteEnabled = 1,
            .ulDepthMode = PL_COMPARE_MODE_GREATER_OR_EQUAL,
            .ulCullMode = PL_CULL_MODE_CULL_BACK,
            .ulWireframe = 0,
            .ulStencilMode = PL_COMPARE_MODE_ALWAYS,
            .ulStencilRef = 0xff,
            .ulStencilMask = 0xff,
            .ulStencilOpFail = PL_STENCIL_OP_KEEP,
            .ulStencilOpDepthFail = PL_STENCIL_OP_KEEP,
            .ulStencilOpPass = PL_STENCIL_OP_KEEP
        },
        .atVertexBufferLayouts = {
            {
                .uByteStride = sizeof(plPlanetVertex),
                .atAttributes = {
                    {.uByteOffset = offsetof(plPlanetVertex, tPositionHigh), .tFormat = PL_VERTEX_FORMAT_FLOAT3},
                    {.uByteOffset = offsetof(plPlanetVertex, tPositionLow),  .tFormat = PL_VERTEX_FORMAT_FLOAT3},
                    {.uByteOffset = offsetof(plPlanetVertex, tNormal),       .tFormat = PL_VERTEX_FORMAT_FLOAT3},
                    {.uByteOffset = offsetof(plPlanetVertex, tUV),           .tFormat = PL_VERTEX_FORMAT_FLOAT2},
                    {.uByteOffset = offsetof(plPlanetVertex, fHeight),       .tFormat = PL_VERTEX_FORMAT_FLOAT}
                }
            }
        },
        .atBlendStates = {
            {
                .bBlendEnabled = false,
                .uColorWriteMask = PL_COLOR_WRITE_MASK_ALL,
                .tSrcColorFactor = PL_BLEND_FACTOR_SRC_ALPHA,
                .tDstColorFactor = PL_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .tColorOp = PL_BLEND_OP_ADD,
                .tSrcAlphaFactor = PL_BLEND_FACTOR_SRC_ALPHA,
                .tDstAlphaFactor = PL_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .tAlphaOp = PL_BLEND_OP_ADD
            }
        },
        .tRenderPassLayout = gptCtx->tRenderPassLayout,
        .pcDebugName = "planet shader"
    };

    plShaderDesc shaderDesc = shaderDescBase;
    view->tShader = gptGfx->create_shader(gptCtx->ptDevice, &shaderDesc);

    shaderDesc.tGraphicsState.ulWireframe = 1;
    shaderDesc.tGraphicsState.ulDepthWriteEnabled = 0;
    shaderDesc.tGraphicsState.ulDepthMode = PL_COMPARE_MODE_ALWAYS;
    shaderDesc.pcDebugName = "planet wireframe shader";
    view->tWireframeShader = gptGfx->create_shader(gptCtx->ptDevice, &shaderDesc);
}

static plPlanetView*
pl_planet_create_view(plPlanet* planet, plCommandBuffer* cmdBuffer, plPlanetViewInit init)
{
    if(!planet || !gptCtx || !gptGfx || !gptCtx->ptDevice || !cmdBuffer || !gptCtx->tRenderPassLayout.uData)
        return NULL;

    plPlanetView* view = (plPlanetView*)PL_ALLOC(sizeof(plPlanetView));
    if(!view)
        return NULL;
    memset(view, 0, sizeof(*view));

    view->ptPlanet = planet;
    view->uOutputWidth = init.uOutputWidth ? init.uOutputWidth : 1280u;
    view->uOutputHeight = init.uOutputHeight ? init.uOutputHeight : 720u;
    view->pcVertexShader = init.pcVertexShader ? init.pcVertexShader : "planet.vert";
    view->pcFragmentShader = init.pcFragmentShader ? init.pcFragmentShader : "planet.frag";
    view->tRuntimeOptions.fLodPixelThreshold = 2.0f;

    const plTextureDesc outputTextureDesc = {
        .tDimensions = {(float)view->uOutputWidth, (float)view->uOutputHeight, 1.0f},
        .tFormat = PL_FORMAT_R8G8B8A8_UNORM,
        .uLayers = 1,
        .uMips = 1,
        .tType = PL_TEXTURE_TYPE_2D,
        .tUsage = PL_TEXTURE_USAGE_SAMPLED | PL_TEXTURE_USAGE_COLOR_ATTACHMENT,
        .pcDebugName = "planet view output"
    };
    view->tOutputTexture = pl__planet_create_texture(cmdBuffer, &outputTextureDesc, "planet view output memory");
    if(!view->tOutputTexture.uData)
        goto fail;
    if(gptDraw)
        view->tOutputTextureHandle = gptDraw->create_bind_group_for_texture(view->tOutputTexture);

    const plTextureDesc depthTextureDesc = {
        .tDimensions = {(float)view->uOutputWidth, (float)view->uOutputHeight, 1.0f},
        .tFormat = PL_FORMAT_D32_FLOAT_S8_UINT,
        .uLayers = 1,
        .uMips = 1,
        .tType = PL_TEXTURE_TYPE_2D,
        .tUsage = PL_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT,
        .pcDebugName = "planet view depth"
    };
    view->tOutputTextureDepth = pl__planet_create_texture(cmdBuffer, &depthTextureDesc, "planet view depth memory");
    if(!view->tOutputTextureDepth.uData)
        goto fail;

    plBlitEncoder* initEncoder = gptGfx->begin_blit_pass(cmdBuffer);
    gptGfx->set_texture_usage(initEncoder, view->tOutputTexture, PL_TEXTURE_USAGE_SAMPLED, 0);
    gptGfx->set_texture_usage(initEncoder, view->tOutputTextureDepth, PL_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT, 0);
    gptGfx->end_blit_pass(initEncoder);

    plRenderPassAttachments attachmentSets[PL_MAX_FRAMES_IN_FLIGHT] = {0};
    for(uint32_t i = 0; i < gptGfx->get_frames_in_flight(); i++)
    {
        attachmentSets[i].atViewAttachments[0] = view->tOutputTextureDepth;
        attachmentSets[i].atViewAttachments[1] = view->tOutputTexture;
    }

    const plRenderPassDesc renderPassDesc = {
        .tLayout = gptCtx->tRenderPassLayout,
        .tDepthTarget = {
            .tLoadOp = PL_LOAD_OP_CLEAR,
            .tStoreOp = PL_STORE_OP_STORE,
            .tStencilLoadOp = PL_LOAD_OP_CLEAR,
            .tStencilStoreOp = PL_STORE_OP_STORE,
            .tCurrentUsage = PL_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT,
            .tNextUsage = PL_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT,
            .fClearZ = 0.0f
        },
        .atColorTargets = {
            {
                .tLoadOp = PL_LOAD_OP_CLEAR,
                .tStoreOp = PL_STORE_OP_STORE,
                .tCurrentUsage = PL_TEXTURE_USAGE_SAMPLED,
                .tNextUsage = PL_TEXTURE_USAGE_SAMPLED,
                .tClearColor = {0.0f, 0.0f, 0.0f, 1.0f}
            }
        },
        .tDimensions = {.x = (float)view->uOutputWidth, .y = (float)view->uOutputHeight},
        .pcDebugName = "planet view"
    };
    view->tRenderPass = gptGfx->create_render_pass(gptCtx->ptDevice, &renderPassDesc, attachmentSets);
    if(!view->tRenderPass.uData)
        goto fail;

    pl_planet_load_shaders(view);
    return view;

fail:
    if(view->tRenderPass.uData)
        gptGfx->destroy_render_pass(gptCtx->ptDevice, view->tRenderPass);
    if(view->tOutputTexture.uData)
        gptGfx->destroy_texture(gptCtx->ptDevice, view->tOutputTexture);
    if(view->tOutputTextureDepth.uData)
        gptGfx->destroy_texture(gptCtx->ptDevice, view->tOutputTextureDepth);
    PL_FREE(view);
    return NULL;
}

static void
pl_planet_cleanup_view(plPlanetView* view)
{
    if(!view)
        return;

    if(gptCtx && gptGfx && gptCtx->ptDevice)
    {
        if(gptGfx->is_shader_valid(gptCtx->ptDevice, view->tShader))
            gptGfx->queue_shader_for_deletion(gptCtx->ptDevice, view->tShader);
        if(gptGfx->is_shader_valid(gptCtx->ptDevice, view->tWireframeShader))
            gptGfx->queue_shader_for_deletion(gptCtx->ptDevice, view->tWireframeShader);
        if(view->tRenderPass.uData)
            gptGfx->destroy_render_pass(gptCtx->ptDevice, view->tRenderPass);
        if(view->tOutputTexture.uData)
            gptGfx->destroy_texture(gptCtx->ptDevice, view->tOutputTexture);
        if(view->tOutputTextureDepth.uData)
            gptGfx->destroy_texture(gptCtx->ptDevice, view->tOutputTextureDepth);
    }

    PL_FREE(view);
}

typedef struct _plPlanetSelectionContext
{
    plPlanet*  ptPlanet;
    plPlanetView* ptView;
    plCamera*  ptCamera;
    uint8_t*   auVisible;
    uint8_t*   auSelected;
    float      fLodPixelThreshold;
    float      fScreenScale;
    bool       bForceMaxLod;
} plPlanetSelectionContext;

static plVec3d
pl__planet_chunk_center(const plPlanetChunkHeader* header)
{
    return (plVec3d){
        0.5 * (header->tMinBound.x + header->tMaxBound.x),
        0.5 * (header->tMinBound.y + header->tMaxBound.y),
        0.5 * (header->tMinBound.z + header->tMaxBound.z)
    };
}

static double
pl__planet_chunk_radius(const plPlanetChunkHeader* header)
{
    const double dx = header->tMaxBound.x - header->tMinBound.x;
    const double dy = header->tMaxBound.y - header->tMinBound.y;
    const double dz = header->tMaxBound.z - header->tMinBound.z;
    return 0.5 * sqrt(dx * dx + dy * dy + dz * dz);
}

static int32_t
pl__planet_find_gpu_tile_index(const plPlanet* planet, plPlanetTileAddress address)
{
    if(!planet || !planet->atGpuTiles)
        return -1;

    uint32_t lo = 0;
    uint32_t hi = planet->uGpuTileCount;
    while(lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const int cmp = pl__planet_compare_address(planet->atGpuTiles[mid].tTile.tAddress, address);
        if(cmp == 0)
            return (int32_t)mid;
        if(cmp < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return -1;
}

static bool
pl__planet_gpu_tile_visible(const plPlanet* planet, const plPlanetGpuTile* gpuTile, const plCamera* camera)
{
    if(!planet || !gpuTile || !camera)
        return false;

    const plPlanetChunkHeader* header = &gpuTile->tHeader;
    const plVec3d center = pl__planet_chunk_center(header);
    const double tileRadius = pl__planet_chunk_radius(header);
    const double planetRadius = planet->ptManifest ? planet->ptManifest->tInit.dRadius : header->dRadius;

    const plVec3d cameraPos = camera->tPosDouble;
    const double cameraLen = sqrt(cameraPos.x * cameraPos.x + cameraPos.y * cameraPos.y + cameraPos.z * cameraPos.z);
    const double centerLen = sqrt(center.x * center.x + center.y * center.y + center.z * center.z);

    if(planetRadius > 0.0 && cameraLen > planetRadius * 1.001 && centerLen > 0.0)
    {
        const double dot = (center.x * cameraPos.x + center.y * cameraPos.y + center.z * cameraPos.z) / (centerLen * cameraLen);
        const double angularPad = tileRadius / planetRadius;
        const double horizon = planetRadius / cameraLen;
        if(dot < horizon - angularPad)
            return false;
    }

    // Tile visibility feeds LOD coverage, not just draw submission. Keep it in
    // planet space so chunks that are only partially inside a render view still
    // refine normally; the viewport/scissor clips the final pixels.
    return true;
}

static float
pl__planet_gpu_tile_screen_error(const plPlanetSelectionContext* ctx, const plPlanetGpuTile* gpuTile)
{
    const plVec3d center = pl__planet_chunk_center(&gpuTile->tHeader);
    const double tileRadius = pl__planet_chunk_radius(&gpuTile->tHeader);
    const plVec3d cameraPos = ctx->ptCamera->tPosDouble;
    const double dx = center.x - cameraPos.x;
    const double dy = center.y - cameraPos.y;
    const double dz = center.z - cameraPos.z;
    double distance = sqrt(dx * dx + dy * dy + dz * dz) - tileRadius;
    if(distance < 1.0)
        distance = 1.0;
    return (float)(gpuTile->tTile.dGeometricError * (double)ctx->fScreenScale / distance);
}

static bool
pl__planet_select_tile_recursive(plPlanetSelectionContext* ctx, plPlanetTileAddress address)
{
    const int32_t tileIndex = pl__planet_find_gpu_tile_index(ctx->ptPlanet, address);
    if(tileIndex < 0)
        return false;
    if(!ctx->auVisible[tileIndex])
        return false;

    plPlanetGpuTile* gpuTile = &ctx->ptPlanet->atGpuTiles[tileIndex];
    const float screenError = pl__planet_gpu_tile_screen_error(ctx, gpuTile);
    bool refined = false;

    if(address.uLod < ctx->ptPlanet->ptManifest->tInit.uMaxLod && (ctx->bForceMaxLod || screenError > ctx->fLodPixelThreshold))
    {
        uint32_t visibleChildCount = 0;
        uint32_t selectedChildCount = 0;
        for(uint32_t childY = 0; childY < 2u; childY++)
        {
            for(uint32_t childX = 0; childX < 2u; childX++)
            {
                const plPlanetTileAddress childAddress = {
                    .tFace = address.tFace,
                    .uLod = (uint8_t)(address.uLod + 1u),
                    .uX = address.uX * 2u + childX,
                    .uY = address.uY * 2u + childY
                };
                const int32_t childIndex = pl__planet_find_gpu_tile_index(ctx->ptPlanet, childAddress);
                if(childIndex < 0 || !ctx->auVisible[childIndex])
                    continue;
                visibleChildCount++;
                if(pl__planet_select_tile_recursive(ctx, childAddress))
                    selectedChildCount++;
            }
        }
        refined = visibleChildCount > 0 && selectedChildCount == visibleChildCount;
    }

    if(!refined)
        ctx->auSelected[tileIndex] = 1u;
    return true;
}

static void
pl__planet_select_visible_tiles(plPlanetView* view, plCamera* camera, uint8_t* selected, uint8_t* visible)
{
    plPlanet* planet = view->ptPlanet;
    for(uint32_t i = 0; i < planet->uGpuTileCount; i++)
        visible[i] = pl__planet_gpu_tile_visible(planet, &planet->atGpuTiles[i], camera) ? 1u : 0u;

    float lodThreshold = view->tRuntimeOptions.fLodPixelThreshold;
    if(lodThreshold <= 0.0f)
        lodThreshold = 2.0f;

    const float tanHalfFov = camera->fFieldOfView > 0.0f ? tanf(0.5f * camera->fFieldOfView) : 1.0f;
    const plVec3d cameraPos = camera->tPosDouble;
    const double cameraLen = sqrt(cameraPos.x * cameraPos.x + cameraPos.y * cameraPos.y + cameraPos.z * cameraPos.z);
    const double planetRadius = planet->ptManifest ? planet->ptManifest->tInit.dRadius : 0.0;
    const double altitude = planetRadius > 0.0 ? cameraLen - planetRadius : DBL_MAX;
    const bool forceMaxLod = planetRadius > 0.0 && altitude >= 0.0 && altitude < planetRadius * 0.1;

    plPlanetSelectionContext ctx = {
        .ptPlanet = planet,
        .ptView = view,
        .ptCamera = camera,
        .auVisible = visible,
        .auSelected = selected,
        .fLodPixelThreshold = lodThreshold,
        .fScreenScale = tanHalfFov > 0.0f ? (float)view->uOutputHeight / (2.0f * tanHalfFov) : (float)view->uOutputHeight,
        .bForceMaxLod = forceMaxLod
    };

    for(int face = 0; face < PL_PLANET_FACE_COUNT; face++)
    {
        const plPlanetTileAddress root = {
            .tFace = face,
            .uLod = 0,
            .uX = 0,
            .uY = 0
        };
        pl__planet_select_tile_recursive(&ctx, root);
    }

    bool anySelected = false;
    for(uint32_t i = 0; i < planet->uGpuTileCount; i++)
    {
        if(selected[i])
        {
            anySelected = true;
            break;
        }
    }

    if(!anySelected)
    {
        for(uint32_t i = 0; i < planet->uGpuTileCount; i++)
            selected[i] = visible[i];
    }
}

static bool
pl__planet_acquire_cache_slot(plPlanet* planet, const uint8_t* selected, uint32_t* outSlot)
{
    if(outSlot)
        *outSlot = UINT32_MAX;
    if(!planet || !outSlot || !planet->aiTileIndexBySlot || planet->uResidentCapacity == 0)
        return false;

    for(uint32_t slot = 0; slot < planet->uResidentCapacity; slot++)
    {
        if(planet->aiTileIndexBySlot[slot] < 0)
        {
            *outSlot = slot;
            return true;
        }
    }

    uint32_t bestSlot = UINT32_MAX;
    uint64_t bestFrame = UINT64_MAX;
    for(uint32_t slot = 0; slot < planet->uResidentCapacity; slot++)
    {
        const int32_t tileIndex = planet->aiTileIndexBySlot[slot];
        if(tileIndex < 0)
            continue;
        if(selected && selected[tileIndex])
            continue;

        const uint64_t lastUsed = planet->atGpuTiles[tileIndex].ulLastUsedFrame;
        if(lastUsed < bestFrame)
        {
            bestFrame = lastUsed;
            bestSlot = slot;
        }
    }

    if(bestSlot == UINT32_MAX)
        return false;

    const int32_t evictedIndex = planet->aiTileIndexBySlot[bestSlot];
    if(evictedIndex >= 0)
    {
        plPlanetGpuTile* evictedTile = &planet->atGpuTiles[evictedIndex];
        evictedTile->bResident = false;
        evictedTile->uCacheSlot = UINT32_MAX;
        evictedTile->uVertexStart = 0;
        evictedTile->uIndexStart = 0;
        planet->aiTileIndexBySlot[bestSlot] = -1;
        if(planet->uResidentCount > 0)
            planet->uResidentCount--;
        planet->tStats.uEvictedTilesLastFrame++;
    }

    *outSlot = bestSlot;
    return true;
}

static bool
pl__planet_upload_tile_to_slot(plPlanet* planet, uint32_t tileIndex, uint32_t slot, plBlitEncoder* encoder, plBufferHandle* outStagingHandle)
{
    if(outStagingHandle)
        *outStagingHandle = (plBufferHandle){0};
    if(!planet || tileIndex >= planet->uGpuTileCount || slot >= planet->uResidentCapacity || !encoder || !outStagingHandle)
        return false;

    plPlanetGpuTile* gpuTile = &planet->atGpuTiles[tileIndex];
    const size_t vertexBytes = (size_t)gpuTile->tHeader.uVertexCount * sizeof(plPlanetVertex);
    const size_t indexBytes = (size_t)gpuTile->tHeader.uIndexCount * sizeof(uint32_t);
    const size_t uploadBytes = vertexBytes + indexBytes;
    if(vertexBytes == 0 || indexBytes == 0 || uploadBytes < vertexBytes)
        return false;
    if(vertexBytes > planet->szSlotVertexBytes || indexBytes > planet->szSlotIndexBytes)
        return false;

    char resolvedPath[PL_PLANET_PATH_MAX] = {0};
    if(!pl_planet_resolve_tile_path(planet->ptManifest, &gpuTile->tTile, resolvedPath, sizeof(resolvedPath)))
        return false;

    const plBufferDesc stagingBufferDesc = {
        .tUsage = PL_BUFFER_USAGE_TRANSFER_SOURCE,
        .szByteSize = uploadBytes,
        .pcDebugName = "planet stream staging buffer"
    };
    plBuffer* stagingBuffer = NULL;
    const plBufferHandle stagingHandle = gptGfx->create_buffer(gptCtx->ptDevice, &stagingBufferDesc, &stagingBuffer);
    if(!stagingHandle.uData || !stagingBuffer)
        return false;

    const plDeviceMemoryAllocation stagingAllocation = gptGfx->allocate_memory(
        gptCtx->ptDevice,
        stagingBuffer->tMemoryRequirements.ulSize,
        PL_MEMORY_FLAGS_HOST_VISIBLE | PL_MEMORY_FLAGS_HOST_COHERENT,
        stagingBuffer->tMemoryRequirements.uMemoryTypeBits,
        "planet stream staging memory");
    gptGfx->bind_buffer_to_memory(gptCtx->ptDevice, stagingHandle, &stagingAllocation);

    char* stagingBytes = stagingBuffer->tMemoryAllocation.pHostMapped;
    if(!stagingBytes)
    {
        gptGfx->queue_buffer_for_deletion(gptCtx->ptDevice, stagingHandle);
        return false;
    }

    plPlanetVertex* vertices = (plPlanetVertex*)stagingBytes;
    uint32_t* indices = (uint32_t*)(stagingBytes + vertexBytes);
    if(!pl_planet_read_chunk_payload(resolvedPath, &gpuTile->tHeader, vertices, indices))
    {
        gptGfx->queue_buffer_for_deletion(gptCtx->ptDevice, stagingHandle);
        return false;
    }

    const uint64_t vertexDestinationOffset = (uint64_t)slot * (uint64_t)planet->szSlotVertexBytes;
    const uint64_t indexDestinationOffset = (uint64_t)slot * (uint64_t)planet->szSlotIndexBytes;
    gptGfx->copy_buffer(encoder, stagingHandle, planet->tVertexBuffer, 0, vertexDestinationOffset, vertexBytes);
    gptGfx->copy_buffer(encoder, stagingHandle, planet->tIndexBuffer, vertexBytes, indexDestinationOffset, indexBytes);

    gpuTile->uCacheSlot = slot;
    gpuTile->uVertexStart = (uint32_t)(vertexDestinationOffset / sizeof(plPlanetVertex));
    gpuTile->uIndexStart = (uint32_t)(indexDestinationOffset / sizeof(uint32_t));
    gpuTile->uIndexCount = gpuTile->tHeader.uIndexCount;
    gpuTile->ulLastUsedFrame = planet->ulFrameCounter;
    gpuTile->bResident = true;
    planet->aiTileIndexBySlot[slot] = (int32_t)tileIndex;
    planet->uResidentCount++;
    planet->tStats.uStreamedTilesLastFrame++;
    *outStagingHandle = stagingHandle;
    return true;
}

static bool
pl__planet_prepare_selected_tiles(plPlanet* planet, uint8_t* selected, plCommandBuffer* cmdBuffer)
{
    if(!planet || !selected || !cmdBuffer || !gptGfx || !gptCtx || !gptCtx->ptDevice)
        return false;

    uint32_t selectedCount = 0;
    uint32_t missingCount = 0;
    for(uint32_t i = 0; i < planet->uGpuTileCount; i++)
    {
        if(!selected[i])
            continue;
        selectedCount++;
        if(!planet->atGpuTiles[i].bResident)
            missingCount++;
        else
            planet->atGpuTiles[i].ulLastUsedFrame = planet->ulFrameCounter;
    }

    if(selectedCount == 0)
    {
        planet->tStats.uLoadedTiles = planet->uResidentCount;
        return true;
    }
    if(missingCount == 0)
    {
        planet->tStats.uLoadedTiles = planet->uResidentCount;
        return true;
    }

    plBufferHandle* stagingHandles = (plBufferHandle*)PL_ALLOC((size_t)missingCount * sizeof(plBufferHandle));
    if(!stagingHandles)
        return false;
    uint32_t stagingCount = 0;

    plBlitEncoder* encoder = gptGfx->begin_blit_pass(cmdBuffer);
    gptGfx->pipeline_barrier_blit(
        encoder,
        PL_PIPELINE_STAGE_VERTEX_SHADER | PL_PIPELINE_STAGE_TRANSFER,
        PL_ACCESS_SHADER_READ | PL_ACCESS_TRANSFER_READ,
        PL_PIPELINE_STAGE_TRANSFER,
        PL_ACCESS_TRANSFER_WRITE);

    bool ok = true;
    for(uint32_t i = 0; i < planet->uGpuTileCount; i++)
    {
        if(!selected[i] || planet->atGpuTiles[i].bResident)
            continue;

        uint32_t slot = UINT32_MAX;
        if(!pl__planet_acquire_cache_slot(planet, selected, &slot))
        {
            selected[i] = 0u;
            ok = false;
            continue;
        }

        plBufferHandle stagingHandle = {0};
        if(!pl__planet_upload_tile_to_slot(planet, i, slot, encoder, &stagingHandle))
        {
            selected[i] = 0u;
            ok = false;
            continue;
        }
        stagingHandles[stagingCount++] = stagingHandle;
    }

    gptGfx->pipeline_barrier_blit(
        encoder,
        PL_PIPELINE_STAGE_TRANSFER,
        PL_ACCESS_TRANSFER_WRITE,
        PL_PIPELINE_STAGE_VERTEX_SHADER | PL_PIPELINE_STAGE_TRANSFER,
        PL_ACCESS_SHADER_READ | PL_ACCESS_TRANSFER_READ);
    gptGfx->end_blit_pass(encoder);

    for(uint32_t i = 0; i < stagingCount; i++)
        gptGfx->queue_buffer_for_deletion(gptCtx->ptDevice, stagingHandles[i]);
    PL_FREE(stagingHandles);

    planet->tStats.uLoadedTiles = planet->uResidentCount;
    return ok;
}

static void
pl_planet_render_view(plPlanetView* view, plCamera* camera, plCommandBuffer* cmdBuffer)
{
    if(!view || !view->ptPlanet || !camera || !cmdBuffer || !gptCtx || !gptGfx || !gptCtx->ptDevice)
        return;

    plPlanet* planet = view->ptPlanet;
    planet->ulFrameCounter++;
    planet->tStats.uDrawnTilesLastFrame = 0;
    planet->tStats.uStreamedTilesLastFrame = 0;
    planet->tStats.uEvictedTilesLastFrame = 0;
    memset(planet->tStats.auDrawnTilesByLod, 0, sizeof(planet->tStats.auDrawnTilesByLod));
    planet->tStats.uManifestTiles = planet->uGpuTileCount;
    planet->tStats.uLoadedTiles = planet->uResidentCount;

    const bool wireframe = (view->tRuntimeOptions.tFlags & PL_PLANET_RENDER_FLAGS_WIREFRAME) != 0;
    const plShaderHandle shader = wireframe ? view->tWireframeShader : view->tShader;
    if(!gptGfx->is_shader_valid(gptCtx->ptDevice, shader))
        return;

    uint8_t* selectedTiles = (uint8_t*)PL_ALLOC((size_t)planet->uGpuTileCount);
    uint8_t* visibleTiles = (uint8_t*)PL_ALLOC((size_t)planet->uGpuTileCount);
    if(!selectedTiles || !visibleTiles)
    {
        if(selectedTiles) PL_FREE(selectedTiles);
        if(visibleTiles) PL_FREE(visibleTiles);
        return;
    }
    memset(selectedTiles, 0, (size_t)planet->uGpuTileCount);
    memset(visibleTiles, 0, (size_t)planet->uGpuTileCount);
    pl__planet_select_visible_tiles(view, camera, selectedTiles, visibleTiles);
    pl__planet_prepare_selected_tiles(planet, selectedTiles, cmdBuffer);

    gptCtx->tCurrentDynamicBufferBlock = gptGfx->allocate_dynamic_data_block(gptCtx->ptDevice);

    plRenderEncoder* encoder = gptGfx->begin_render_pass(cmdBuffer, view->tRenderPass, NULL);

    const plRenderViewport viewport = {
        .fWidth = (float)view->uOutputWidth,
        .fHeight = (float)view->uOutputHeight,
        .fMinDepth = 0.0f,
        .fMaxDepth = 1.0f
    };
    gptGfx->set_viewport(encoder, &viewport);

    const plScissor scissor = {
        .uWidth = view->uOutputWidth,
        .uHeight = view->uOutputHeight
    };
    gptGfx->set_scissor_region(encoder, &scissor);
    gptGfx->set_depth_bias(encoder, 0.0f, 0.0f, 0.0f);
    gptGfx->bind_shader(encoder, shader);
    gptGfx->bind_vertex_buffer(encoder, planet->tVertexBuffer);

    const plMat4 viewProjection = pl_mul_mat4(&camera->tProjMat, &camera->tViewMatDouble);
    const int shaderFlags = pl__planet_to_shader_flags(view->tRuntimeOptions.tFlags);

    for(uint32_t i = 0; i < planet->uGpuTileCount; i++)
    {
        if(!selectedTiles[i])
            continue;

        const plPlanetGpuTile* gpuTile = &planet->atGpuTiles[i];
        if(!gpuTile->bResident)
            continue;

        plDynamicBinding dynamicBinding = pl_allocate_dynamic_data(gptGfx, gptCtx->ptDevice, &gptCtx->tCurrentDynamicBufferBlock);
        plGpuDynPlanetData* dynamic = (plGpuDynPlanetData*)dynamicBinding.pcData;
        memset(dynamic, 0, sizeof(*dynamic));

        dynamic->iLevel = (int)gpuTile->tTile.tAddress.uLod;
        dynamic->tFlags = shaderFlags;
        dynamic->uTextureIndex = 0;
        dynamic->iChunkID = (int)i;
        dynamic->tUVInfo = (plVec4){1.0f, 1.0f, 0.0f, 0.0f};
        dynamic->tLightDirection = planet->tRuntimeOptions.tLightDirection;
        dynamic->fRadius = (float)planet->ptManifest->tInit.dRadius;
        dynamic->fHazardMapStrength = 0.0f;
        pl__planet_split_double(camera->tPosDouble.x, &dynamic->tCameraPosHigh.x, &dynamic->tCameraPosLow.x);
        pl__planet_split_double(camera->tPosDouble.y, &dynamic->tCameraPosHigh.y, &dynamic->tCameraPosLow.y);
        pl__planet_split_double(camera->tPosDouble.z, &dynamic->tCameraPosHigh.z, &dynamic->tCameraPosLow.z);
        dynamic->tCameraViewProjection = viewProjection;

        gptGfx->bind_graphics_bind_groups(encoder, shader, 0, 0, NULL, 1, &dynamicBinding);

        const plDrawIndex draw = {
            .uInstanceCount = 1,
            .uIndexCount = gpuTile->uIndexCount,
            .uVertexStart = gpuTile->uVertexStart,
            .uIndexStart = gpuTile->uIndexStart,
            .tIndexBuffer = planet->tIndexBuffer
        };
        gptGfx->draw_indexed(encoder, 1, &draw);
        planet->tStats.uDrawnTilesLastFrame++;
        if(gpuTile->tTile.tAddress.uLod < 16u)
            planet->tStats.auDrawnTilesByLod[gpuTile->tTile.tAddress.uLod]++;
    }

    gptGfx->end_render_pass(encoder);
    PL_FREE(visibleTiles);
    PL_FREE(selectedTiles);
}

static plBindGroupHandle
pl_planet_get_view_texture(plPlanetView* view)
{
    return view ? view->tOutputTextureHandle : (plBindGroupHandle){0};
}

static plTextureHandle
pl_planet_get_view_output_texture(plPlanetView* view)
{
    return view ? view->tOutputTexture : (plTextureHandle){0};
}

static void
pl_planet_set_runtime_options(plPlanet* planet, plPlanetRuntimeOptions options)
{
    if(planet)
        planet->tRuntimeOptions = options;
}

static plPlanetRuntimeOptions
pl_planet_get_runtime_options(plPlanet* planet)
{
    return planet ? planet->tRuntimeOptions : (plPlanetRuntimeOptions){0};
}

static void
pl_planet_set_view_runtime_options(plPlanetView* view, plPlanetViewRuntimeOptions options)
{
    if(view)
        view->tRuntimeOptions = options;
}

static plPlanetViewRuntimeOptions
pl_planet_get_view_runtime_options(plPlanetView* view)
{
    return view ? view->tRuntimeOptions : (plPlanetViewRuntimeOptions){0};
}

static void
pl_planet_set_shaders(plPlanetView* view, const char* vertexShader, const char* fragmentShader)
{
    if(!view)
        return;
    if(vertexShader)
        view->pcVertexShader = vertexShader;
    if(fragmentShader)
        view->pcFragmentShader = fragmentShader;
    pl_planet_load_shaders(view);
}

static plPlanetRenderStats
pl_planet_get_render_stats(plPlanet* planet)
{
    return planet ? planet->tStats : (plPlanetRenderStats){0};
}

static bool
pl_planet_choose_source(const plPlanetManifest* manifest, double latitude, double longitude, double desiredMetersPerPixel, uint32_t* outSourceIndex)
{
    if(outSourceIndex)
        *outSourceIndex = UINT32_MAX;
    if(!manifest || !outSourceIndex)
        return false;

    bool found = false;
    uint32_t bestIndex = UINT32_MAX;
    int32_t bestPriority = INT32_MIN;
    double bestMetersPerPixel = DBL_MAX;
    double bestResolutionDelta = DBL_MAX;

    for(uint32_t i = 0; i < manifest->uSourceCount; i++)
    {
        const plPlanetSourceRecord* source = &manifest->atSources[i];
        if(!pl__planet_source_contains(source, latitude, longitude))
            continue;

        const double sourceMpp = source->dNativeMetersPerPixel > 0.0 ? source->dNativeMetersPerPixel : DBL_MAX;
        const double resolutionDelta = desiredMetersPerPixel > 0.0 ? pl__planet_absd(sourceMpp - desiredMetersPerPixel) : 0.0;

        bool better = !found;
        if(found && source->uPriority > bestPriority)
            better = true;
        else if(found && source->uPriority == bestPriority && sourceMpp < bestMetersPerPixel)
            better = true;
        else if(found && source->uPriority == bestPriority && sourceMpp == bestMetersPerPixel && resolutionDelta < bestResolutionDelta)
            better = true;

        if(better)
        {
            found = true;
            bestIndex = i;
            bestPriority = source->uPriority;
            bestMetersPerPixel = sourceMpp;
            bestResolutionDelta = resolutionDelta;
        }
    }

    if(!found)
        return false;

    *outSourceIndex = bestIndex;
    return true;
}

static plVec3d
pl_planet_face_uv_to_direction(plPlanetFace face, double u, double v)
{
    const double s = pl__planet_clamp(u, 0.0, 1.0) * 2.0 - 1.0;
    const double t = pl__planet_clamp(v, 0.0, 1.0) * 2.0 - 1.0;

    switch(face)
    {
        case PL_PLANET_FACE_POS_X: return pl__planet_norm_vec3d((plVec3d){ 1.0, t, -s});
        case PL_PLANET_FACE_NEG_X: return pl__planet_norm_vec3d((plVec3d){-1.0, t,  s});
        case PL_PLANET_FACE_POS_Y: return pl__planet_norm_vec3d((plVec3d){ s,   1.0, -t});
        case PL_PLANET_FACE_NEG_Y: return pl__planet_norm_vec3d((plVec3d){ s,  -1.0,  t});
        case PL_PLANET_FACE_POS_Z: return pl__planet_norm_vec3d((plVec3d){ s,   t,   1.0});
        case PL_PLANET_FACE_NEG_Z: return pl__planet_norm_vec3d((plVec3d){-s,   t,  -1.0});
        default:                    return (plVec3d){0.0, 0.0, 1.0};
    }
}

static bool
pl_planet_direction_to_face_uv(plVec3d direction, plPlanetFace* outFace, double* outU, double* outV)
{
    const plVec3d d = pl__planet_norm_vec3d(direction);
    const double ax = pl__planet_absd(d.x);
    const double ay = pl__planet_absd(d.y);
    const double az = pl__planet_absd(d.z);

    plPlanetFace face = PL_PLANET_FACE_POS_Z;
    double s = 0.0;
    double t = 0.0;

    if(ax >= ay && ax >= az)
    {
        if(d.x >= 0.0)
        {
            face = PL_PLANET_FACE_POS_X;
            s = -d.z / ax;
            t =  d.y / ax;
        }
        else
        {
            face = PL_PLANET_FACE_NEG_X;
            s =  d.z / ax;
            t =  d.y / ax;
        }
    }
    else if(ay >= ax && ay >= az)
    {
        if(d.y >= 0.0)
        {
            face = PL_PLANET_FACE_POS_Y;
            s =  d.x / ay;
            t = -d.z / ay;
        }
        else
        {
            face = PL_PLANET_FACE_NEG_Y;
            s = d.x / ay;
            t = d.z / ay;
        }
    }
    else
    {
        if(d.z >= 0.0)
        {
            face = PL_PLANET_FACE_POS_Z;
            s = d.x / az;
            t = d.y / az;
        }
        else
        {
            face = PL_PLANET_FACE_NEG_Z;
            s = -d.x / az;
            t =  d.y / az;
        }
    }

    if(outFace)
        *outFace = face;
    if(outU)
        *outU = 0.5 * (s + 1.0);
    if(outV)
        *outV = 0.5 * (t + 1.0);
    return true;
}

//-----------------------------------------------------------------------------
// [SECTION] extension loading
//-----------------------------------------------------------------------------

PL_EXPORT void
pl_load_ext(plApiRegistryI* ptApiRegistry, bool bReload)
{
    const plPlanetI tApi = {
        .initialize = pl_planet_initialize,
        .cleanup = pl_planet_cleanup,
        .create_manifest = pl_planet_create_manifest,
        .cleanup_manifest = pl_planet_cleanup_manifest,
        .add_source = pl_planet_add_source,
        .add_tile = pl_planet_add_tile,
        .finalize_manifest = pl_planet_finalize_manifest,
        .load_manifest_json = pl_planet_load_manifest_json,
        .get_manifest_init = pl_planet_get_manifest_init,
        .get_source_count = pl_planet_get_source_count,
        .get_source = pl_planet_get_source,
        .get_tile_count = pl_planet_get_tile_count,
        .get_tile = pl_planet_get_tile,
        .manifest_has_payloads = pl_planet_manifest_has_payloads,
        .get_manifest_path = pl_planet_get_manifest_path,
        .get_manifest_base_path = pl_planet_get_manifest_base_path,
        .make_address = pl_planet_make_address,
        .parent_address = pl_planet_parent_address,
        .find_tile = pl_planet_find_tile,
        .select_tile = pl_planet_select_tile,
        .resolve_tile_path = pl_planet_resolve_tile_path,
        .choose_source = pl_planet_choose_source,
        .read_chunk_header = pl_planet_read_chunk_header,
        .read_chunk_payload = pl_planet_read_chunk_payload,
        .create_planet = pl_planet_create_planet,
        .cleanup_planet = pl_planet_cleanup_planet,
        .create_view = pl_planet_create_view,
        .cleanup_view = pl_planet_cleanup_view,
        .render_view = pl_planet_render_view,
        .get_view_texture = pl_planet_get_view_texture,
        .get_view_output_texture = pl_planet_get_view_output_texture,
        .set_runtime_options = pl_planet_set_runtime_options,
        .get_runtime_options = pl_planet_get_runtime_options,
        .set_view_runtime_options = pl_planet_set_view_runtime_options,
        .get_view_runtime_options = pl_planet_get_view_runtime_options,
        .reload_shaders = pl_planet_load_shaders,
        .set_shaders = pl_planet_set_shaders,
        .get_render_stats = pl_planet_get_render_stats,
        .face_uv_to_direction = pl_planet_face_uv_to_direction,
        .direction_to_face_uv = pl_planet_direction_to_face_uv
    };
    pl_set_api(ptApiRegistry, plPlanetI, &tApi);

    gptMemory = pl_get_api_latest(ptApiRegistry, plMemoryI);
    gptGfx = pl_get_api_latest(ptApiRegistry, plGraphicsI);
    gptShader = pl_get_api_latest(ptApiRegistry, plShaderI);
    gptDraw = pl_get_api_latest(ptApiRegistry, plDrawI);

    const plDataRegistryI* dataRegistry = pl_get_api_latest(ptApiRegistry, plDataRegistryI);
    if(bReload)
    {
        gptCtx = dataRegistry ? dataRegistry->get_data("plPlanetContext") : gptCtx;
    }
    else
    {
        static plPlanetContext ctx = {0};
        gptCtx = &ctx;
        if(dataRegistry)
            dataRegistry->set_data("plPlanetContext", gptCtx);
    }
}

PL_EXPORT void
pl_unload_ext(plApiRegistryI* ptApiRegistry, bool bReload)
{
    if(bReload)
        return;

    const plPlanetI* ptApi = pl_get_api_latest(ptApiRegistry, plPlanetI);
    ptApiRegistry->remove_api(ptApi);
}

//-----------------------------------------------------------------------------
// [SECTION] unity build
//-----------------------------------------------------------------------------

#define PL_MEMORY_IMPLEMENTATION
#include "pl_memory.h"
