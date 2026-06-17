/*
   pl_planet_ext.h

   Planet is the canonical planet terrain contract. It is centered on a global
   cube-sphere tile pyramid, sparse high-resolution availability, and explicit
   DEM source provenance.
*/

/*
Index of this file:
// [SECTION] header mess
// [SECTION] apis
// [SECTION] includes
// [SECTION] forward declarations
// [SECTION] public api
// [SECTION] structs
// [SECTION] enums
*/

//-----------------------------------------------------------------------------
// [SECTION] header mess
//-----------------------------------------------------------------------------

#ifndef PL_PLANET_EXT_H
#define PL_PLANET_EXT_H

//-----------------------------------------------------------------------------
// [SECTION] apis
//-----------------------------------------------------------------------------

#define plPlanetI_version {0, 4, 0}

//-----------------------------------------------------------------------------
// [SECTION] includes
//-----------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>
#include "pl_math.h"

//-----------------------------------------------------------------------------
// [SECTION] forward declarations
//-----------------------------------------------------------------------------

typedef struct _plPlanetExtInit       plPlanetExtInit;
typedef struct _plPlanetManifestInit  plPlanetManifestInit;
typedef struct _plPlanetManifest      plPlanetManifest;
typedef struct _plPlanetSourceRecord  plPlanetSourceRecord;
typedef struct _plPlanetTileAddress   plPlanetTileAddress;
typedef struct _plPlanetTileRecord    plPlanetTileRecord;
typedef struct _plPlanetTileSelection plPlanetTileSelection;
typedef struct _plPlanetChunkHeader   plPlanetChunkHeader;
typedef struct _plPlanetVertex        plPlanetVertex;
typedef struct _plPlanet              plPlanet;
typedef struct _plPlanetInit          plPlanetInit;
typedef struct _plPlanetView          plPlanetView;
typedef struct _plPlanetViewInit      plPlanetViewInit;
typedef struct _plPlanetRuntimeOptions plPlanetRuntimeOptions;
typedef struct _plPlanetViewRuntimeOptions plPlanetViewRuntimeOptions;
typedef struct _plPlanetRenderStats   plPlanetRenderStats;

typedef int plPlanetFace;
typedef int plPlanetTilingMode;
typedef int plPlanetManifestFlags;
typedef int plPlanetSourceFlags;
typedef int plPlanetTileFlags;
typedef int plPlanetChunkFlags;
typedef int plPlanetRenderFlags;

typedef struct _plDevice        plDevice;        // pl_graphics_ext.h
typedef struct _plCommandBuffer plCommandBuffer; // pl_graphics_ext.h
typedef struct _plCamera        plCamera;        // pl_camera_ext.h
typedef union  plBindGroupHandle plBindGroupHandle; // pl_graphics_ext.h
typedef union  plTextureHandle   plTextureHandle;   // pl_graphics_ext.h

//-----------------------------------------------------------------------------
// [SECTION] public api
//-----------------------------------------------------------------------------

typedef struct _plPlanetI
{
    // setup/shutdown
    void (*initialize)(plPlanetExtInit);
    void (*cleanup)   (void);

    // manifest construction
    plPlanetManifest* (*create_manifest) (plPlanetManifestInit);
    void               (*cleanup_manifest)(plPlanetManifest*);
    uint32_t           (*add_source)      (plPlanetManifest*, plPlanetSourceRecord);
    bool               (*add_tile)        (plPlanetManifest*, plPlanetTileRecord);
    void               (*finalize_manifest)(plPlanetManifest*);

    // manifest loading. Loaded manifests use paths relative to the manifest file.
    plPlanetManifest* (*load_manifest_json)(const char* path, bool validatePayloads);

    // manifest inspection
    const plPlanetManifestInit* (*get_manifest_init)(const plPlanetManifest*);
    uint32_t                   (*get_source_count)(const plPlanetManifest*);
    const plPlanetSourceRecord* (*get_source)   (const plPlanetManifest*, uint32_t sourceIndex);
    uint32_t                   (*get_tile_count)  (const plPlanetManifest*);
    const plPlanetTileRecord* (*get_tile)        (const plPlanetManifest*, uint32_t tileIndex);
    bool                       (*manifest_has_payloads)(const plPlanetManifest*);
    const char*                (*get_manifest_path)    (const plPlanetManifest*);
    const char*                (*get_manifest_base_path)(const plPlanetManifest*);

    // addressing and lookup
    plPlanetTileAddress (*make_address) (plPlanetFace face, uint8_t lod, uint32_t x, uint32_t y);
    plPlanetTileAddress (*parent_address)(plPlanetTileAddress);
    bool                 (*find_tile)    (const plPlanetManifest*, plPlanetTileAddress, const plPlanetTileRecord** outTile);
    bool                 (*select_tile)  (const plPlanetManifest*, plPlanetTileAddress desired, plPlanetTileSelection* outSelection);
    bool                 (*resolve_tile_path)(const plPlanetManifest*, const plPlanetTileRecord*, char* outPath, uint32_t outPathSize);

    // source selection for chunkgen/mosaicking
    bool (*choose_source)(const plPlanetManifest*, double latitude, double longitude, double desiredMetersPerPixel, uint32_t* outSourceIndex);

    // chunk payload loading. The caller owns vertex/index storage so this path can
    // feed CPU staging or GPU upload buffers directly.
    bool (*read_chunk_header) (const char* path, plPlanetChunkHeader* outHeader);
    bool (*read_chunk_payload)(const char* path, const plPlanetChunkHeader* header, plPlanetVertex* outVertices, uint32_t* outIndices);

    // GPU runtime. This renderer keeps full tile metadata on the CPU, streams
    // selected payloads into a bounded GPU cache, and uses view-dependent
    // culling, screen-space LOD selection, and parent fallback.
    plPlanet* (*create_planet) (plCommandBuffer*, plPlanetInit);
    void       (*cleanup_planet)(plPlanet*);

    plPlanetView*   (*create_view)            (plPlanet*, plCommandBuffer*, plPlanetViewInit);
    void             (*cleanup_view)           (plPlanetView*);
    void             (*render_view)            (plPlanetView*, plCamera*, plCommandBuffer*);
    plBindGroupHandle (*get_view_texture)      (plPlanetView*);
    plTextureHandle   (*get_view_output_texture)(plPlanetView*);

    void                         (*set_runtime_options)     (plPlanet*, plPlanetRuntimeOptions);
    plPlanetRuntimeOptions      (*get_runtime_options)     (plPlanet*);
    void                         (*set_view_runtime_options)(plPlanetView*, plPlanetViewRuntimeOptions);
    plPlanetViewRuntimeOptions  (*get_view_runtime_options)(plPlanetView*);
    void                         (*reload_shaders)          (plPlanetView*);
    void                         (*set_shaders)             (plPlanetView*, const char* vertexShader, const char* fragmentShader);
    plPlanetRenderStats         (*get_render_stats)        (plPlanet*);

    // cube-sphere helpers. u/v are normalized [0, 1] face coordinates.
    plVec3d (*face_uv_to_direction)(plPlanetFace face, double u, double v);
    bool    (*direction_to_face_uv)(plVec3d direction, plPlanetFace* outFace, double* outU, double* outV);
} plPlanetI;

//-----------------------------------------------------------------------------
// [SECTION] structs
//-----------------------------------------------------------------------------

typedef struct _plPlanetExtInit
{
    plDevice* ptDevice;
    uint32_t  uStagingBufferSize; // default: 67108864 bytes
    uint32_t  uGpuCacheSize;      // default: 536870912 bytes
} plPlanetExtInit;

typedef struct _plPlanetInit
{
    const char* pcManifestPath;

    // If ptManifest is supplied, the planet takes ownership of it. If both are
    // supplied, ptManifest wins and pcManifestPath is only diagnostic.
    plPlanetManifest* ptManifest;

    bool bValidatePayloads; // default true when loading from pcManifestPath
} plPlanetInit;

typedef struct _plPlanetViewInit
{
    uint32_t uOutputWidth;
    uint32_t uOutputHeight;

    // shaders
    const char* pcVertexShader;   // default: "planet.vert"
    const char* pcFragmentShader; // default: "planet.frag"
} plPlanetViewInit;

typedef struct _plPlanetRuntimeOptions
{
    plVec3 tLightDirection; // default {-1, -1, -1}
} plPlanetRuntimeOptions;

typedef struct _plPlanetViewRuntimeOptions
{
    plPlanetRenderFlags tFlags;
    float fLodPixelThreshold; // default: 2.0
} plPlanetViewRuntimeOptions;

typedef struct _plPlanetRenderStats
{
    uint32_t uManifestTiles;
    uint32_t uLoadedTiles; // currently resident in the GPU cache
    uint32_t uDrawnTilesLastFrame;
    uint32_t uStreamedTilesLastFrame;
    uint32_t uEvictedTilesLastFrame;
    uint32_t auDrawnTilesByLod[16];
    uint64_t ulVertexBytes;
    uint64_t ulIndexBytes;
} plPlanetRenderStats;

typedef struct _plPlanetManifestInit
{
    double                 dRadius;
    uint32_t               uTileSize;
    uint8_t                uMaxLod;
    plPlanetTilingMode    tTilingMode;
    plPlanetManifestFlags tFlags;
} plPlanetManifestInit;

typedef struct _plPlanetTileAddress
{
    plPlanetFace tFace;
    uint8_t       uLod;
    uint32_t      uX;
    uint32_t      uY;
} plPlanetTileAddress;

typedef struct _plPlanetSourceRecord
{
    char                 acName[64];
    char                 acPath[256];
    plPlanetSourceFlags tFlags;

    // Higher priority wins. When priorities tie, lower native MPP wins.
    int32_t uPriority;
    double  dNativeMetersPerPixel;

    // Geodetic footprint in degrees. Longitudes are normalized to [0, 360);
    // dMinLongitude > dMaxLongitude means the footprint crosses the seam.
    double dMinLatitude;
    double dMaxLatitude;
    double dMinLongitude;
    double dMaxLongitude;

    // Optional vertical range, useful for manifest diagnostics and error budgets.
    double dMinHeight;
    double dMaxHeight;
} plPlanetSourceRecord;

typedef struct _plPlanetTileRecord
{
    plPlanetTileAddress tAddress;
    plPlanetTileFlags   tFlags;
    uint32_t             uSourceIndex;

    // Runtime LOD should use screen-space geometric error derived from this.
    double dGeometricError;
    double dMinHeight;
    double dMaxHeight;

    // Chunk payload location. File may be a standalone chunk or a packed file.
    char     acChunkFile[256];
    uint64_t ulByteOffset;
    uint64_t ulByteSize;
} plPlanetTileRecord;

typedef struct _plPlanetTileSelection
{
    const plPlanetTileRecord* ptTile;
    plPlanetTileAddress      tRequested;
    plPlanetTileAddress      tSelected;
    uint8_t                   uFallbackLevels;
    bool                      bExact;
} plPlanetTileSelection;

typedef struct _plPlanetChunkHeader
{
    uint32_t uMagic;        // PL_PLANET_CHUNK_MAGIC
    uint16_t uVersionMajor; // PL_PLANET_CHUNK_VERSION_MAJOR
    uint16_t uVersionMinor; // PL_PLANET_CHUNK_VERSION_MINOR
    uint32_t uHeaderSize;
    uint32_t uVertexSize;
    uint32_t uFlags;

    int32_t  iFace;
    uint32_t uLod;
    uint32_t uX;
    uint32_t uY;
    uint32_t uSourceIndex; // primary/provenance source selected for this tile
    uint32_t uTileSize;
    uint32_t uVertexCount;
    uint32_t uIndexCount;

    double dRadius;
    double dGeometricError;
    double dMinHeight;
    double dMaxHeight;

    plVec3d tMinBound;
    plVec3d tMaxBound;
    plVec3d tMinBoundFlat;
    plVec3d tMaxBoundFlat;

    uint64_t ulVertexDataOffset;
    uint64_t ulIndexDataOffset;
} plPlanetChunkHeader;

typedef struct _plPlanetVertex
{
    // Split positions are intentionally file-native so planet rendering can
    // keep high precision without reprocessing chunks at load time.
    plVec3 tPositionHigh;
    plVec3 tPositionLow;
    plVec3 tNormal;
    plVec2 tUV;
    float  fHeight;
} plPlanetVertex;

//-----------------------------------------------------------------------------
// [SECTION] enums
//-----------------------------------------------------------------------------

enum _plPlanetFace
{
    PL_PLANET_FACE_POS_X = 0,
    PL_PLANET_FACE_NEG_X,
    PL_PLANET_FACE_POS_Y,
    PL_PLANET_FACE_NEG_Y,
    PL_PLANET_FACE_POS_Z,
    PL_PLANET_FACE_NEG_Z,
    PL_PLANET_FACE_COUNT
};

enum _plPlanetTilingMode
{
    PL_PLANET_TILING_CUBE_SPHERE = 0
};

enum _plPlanetManifestFlags
{
    PL_PLANET_MANIFEST_FLAGS_NONE = 0
};

enum _plPlanetSourceFlags
{
    PL_PLANET_SOURCE_FLAGS_NONE = 0,
    PL_PLANET_SOURCE_FLAGS_HAS_NODATA = 1 << 0,
    PL_PLANET_SOURCE_FLAGS_GLOBAL_LONGITUDE = 1 << 1
};

enum _plPlanetTileFlags
{
    PL_PLANET_TILE_FLAGS_NONE = 0,
    PL_PLANET_TILE_FLAGS_AVAILABLE = 1 << 0
};

enum _plPlanetChunkConstants
{
    PL_PLANET_CHUNK_MAGIC = 0x32435450, // "PTC2" in little-endian files
    PL_PLANET_CHUNK_VERSION_MAJOR = 0,
    PL_PLANET_CHUNK_VERSION_MINOR = 1
};

enum _plPlanetChunkFlags
{
    PL_PLANET_CHUNK_FLAGS_NONE = 0,
    PL_PLANET_CHUNK_FLAGS_HEIGHTS_ARE_RADIAL_DISTANCE = 1 << 0,
    PL_PLANET_CHUNK_FLAGS_HAS_SKIRTS = 1 << 1
};

enum _plPlanetRenderFlags
{
    PL_PLANET_RENDER_FLAGS_NONE        = 0,
    PL_PLANET_RENDER_FLAGS_WIREFRAME   = 1 << 0,
    PL_PLANET_RENDER_FLAGS_SHOW_LEVELS = 1 << 1,
    PL_PLANET_RENDER_FLAGS_SHOW_TILES  = 1 << 2,
    PL_PLANET_RENDER_FLAGS_FLATTEN     = 1 << 3
};

#endif // PL_PLANET_EXT_H
