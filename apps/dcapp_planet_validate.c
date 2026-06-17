/*
   dcapp_planet_validate.c

   Runtime-side validator for planet manifests and .p2c payloads.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pl.h"
#include "pl_planet_ext.h"

static const plIOI      *_ext_ioi     = NULL;
static const plPlanetI *_ext_planet = NULL;

static void
_show_help(void)
{
    printf("dcapp-planet-validate - Validate a planet manifest through pl_planet_ext\n\n");
    printf("Usage:\n");
    printf("  dcapp-planet-validate <manifest.planet.json> [options]\n\n");
    printf("Options:\n");
    printf("  --no-payload-validate   Load manifest without checking every .p2c header\n");
    printf("  --read-first            Read the first payload's vertex/index data\n");
    printf("  --planet-validate-help Show this help without invoking Pilotlight help\n");
}

PL_EXPORT void*
pl_app_load(plApiRegistryI *api_registry, void *app_data)
{
    if(app_data)
        return app_data;

    const plExtensionRegistryI *extension_registry = pl_get_api_latest(api_registry, plExtensionRegistryI);
    extension_registry->load("pl_unity_ext", NULL, NULL, true);
    extension_registry->load("pl_platform_ext", "pl_load_platform_ext", "pl_unload_platform_ext", false);
    extension_registry->load("pl_planet_ext", NULL, NULL, true);

    _ext_ioi = pl_get_api_latest(api_registry, plIOI);
    _ext_planet = pl_get_api_latest(api_registry, plPlanetI);
    plIO *io = _ext_ioi->get_io();
    if(!_ext_planet)
    {
        fprintf(stderr, "Error: failed to get plPlanetI from registry\n");
        io->bRunning = false;
        return NULL;
    }

    int argc = io->iArgc - 3;
    char **argv = io->apArgv + 3;
    const char *manifest_path = NULL;
    bool validate_payloads = true;
    bool read_first = false;

    for(int i = 0; i < argc; i++)
    {
        if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "--planet-validate-help") == 0)
        {
            _show_help();
            io->bRunning = false;
            return NULL;
        }
        else if(strcmp(argv[i], "--no-payload-validate") == 0)
        {
            validate_payloads = false;
        }
        else if(strcmp(argv[i], "--read-first") == 0)
        {
            read_first = true;
        }
        else if(argv[i][0] == '-')
        {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            io->bRunning = false;
            return NULL;
        }
        else if(!manifest_path)
        {
            manifest_path = argv[i];
        }
        else
        {
            fprintf(stderr, "Error: unexpected argument: %s\n", argv[i]);
            io->bRunning = false;
            return NULL;
        }
    }

    if(!manifest_path)
    {
        fprintf(stderr, "Error: manifest path is required\n");
        _show_help();
        io->bRunning = false;
        return NULL;
    }

    plPlanetManifest *manifest = _ext_planet->load_manifest_json(manifest_path, validate_payloads);
    if(!manifest)
    {
        fprintf(stderr, "Error: failed to load planet manifest: %s\n", manifest_path);
        io->bRunning = false;
        return NULL;
    }

    const plPlanetManifestInit *init = _ext_planet->get_manifest_init(manifest);
    const uint32_t source_count = _ext_planet->get_source_count(manifest);
    const uint32_t tile_count = _ext_planet->get_tile_count(manifest);

    printf("========================================\n");
    printf("dcapp-planet-validate\n");
    printf("========================================\n");
    printf("Manifest: %s\n", _ext_planet->get_manifest_path(manifest));
    printf("Base path: %s\n", _ext_planet->get_manifest_base_path(manifest));
    printf("Radius: %.3f\n", init ? init->dRadius : 0.0);
    printf("Tile size: %u\n", init ? init->uTileSize : 0u);
    printf("Max LOD: %u\n", init ? init->uMaxLod : 0u);
    printf("Payloads: %s\n", _ext_planet->manifest_has_payloads(manifest) ? "yes" : "no");
    printf("Sources: %u\n", source_count);
    printf("Tiles: %u\n", tile_count);

    if(source_count > 0)
    {
        const plPlanetSourceRecord *source = _ext_planet->get_source(manifest, 0);
        printf("First source: %s mpp=%.3f priority=%d flags=0x%x\n",
               source ? source->acName : "(null)",
               source ? source->dNativeMetersPerPixel : 0.0,
               source ? source->uPriority : 0,
               source ? source->tFlags : 0);
    }

    if(tile_count > 0)
    {
        const plPlanetTileRecord *tile = _ext_planet->get_tile(manifest, 0);
        char path[512];
        if(tile && _ext_planet->resolve_tile_path(manifest, tile, path, sizeof(path)))
        {
            printf("First tile: face=%d lod=%u x=%u y=%u source=%u file=%s bytes=%llu\n",
                   tile->tAddress.tFace,
                   tile->tAddress.uLod,
                   tile->tAddress.uX,
                   tile->tAddress.uY,
                   tile->uSourceIndex,
                   path,
                   (unsigned long long)tile->ulByteSize);

            if(_ext_planet->manifest_has_payloads(manifest))
            {
                plPlanetChunkHeader header = {0};
                if(!_ext_planet->read_chunk_header(path, &header))
                {
                    fprintf(stderr, "Error: failed to read first chunk header: %s\n", path);
                    _ext_planet->cleanup_manifest(manifest);
                    io->bRunning = false;
                    return NULL;
                }
                printf("First header: vertices=%u indices=%u header=%u vertex_size=%u\n",
                       header.uVertexCount,
                       header.uIndexCount,
                       header.uHeaderSize,
                       header.uVertexSize);

                if(read_first)
                {
                    plPlanetVertex *vertices = (plPlanetVertex *)malloc((size_t)header.uVertexCount * sizeof(plPlanetVertex));
                    uint32_t *indices = (uint32_t *)malloc((size_t)header.uIndexCount * sizeof(uint32_t));
                    if(!vertices || !indices || !_ext_planet->read_chunk_payload(path, &header, vertices, indices))
                    {
                        fprintf(stderr, "Error: failed to read first chunk payload: %s\n", path);
                        free(vertices);
                        free(indices);
                        _ext_planet->cleanup_manifest(manifest);
                        io->bRunning = false;
                        return NULL;
                    }
                    printf("First payload: v0=(%.3f %.3f %.3f)+(%.9f %.9f %.9f) i0=%u\n",
                           vertices[0].tPositionHigh.x,
                           vertices[0].tPositionHigh.y,
                           vertices[0].tPositionHigh.z,
                           vertices[0].tPositionLow.x,
                           vertices[0].tPositionLow.y,
                           vertices[0].tPositionLow.z,
                           indices[0]);
                    free(vertices);
                    free(indices);
                }
            }
        }
    }

    _ext_planet->cleanup_manifest(manifest);
    printf("Done.\n");
    io->bRunning = false;
    return NULL;
}

PL_EXPORT void
pl_app_shutdown(void *app_data)
{
    (void)app_data;
}

PL_EXPORT void
pl_app_resize(plWindow *window, void *app_data)
{
    (void)window;
    (void)app_data;
}

PL_EXPORT void
pl_app_update(void *app_data)
{
    (void)app_data;
}
