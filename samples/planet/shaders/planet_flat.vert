#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "pl_shader_interop_planet.h"

layout(location = 0) in vec3 inHighPos;
layout(location = 1) in vec3 inLowPos;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;
layout(location = 4) in float inHeight;

layout(location = 0) out struct plShaderOut {
    vec4 tColor;
    vec3 tWorldPosition;
    vec3 tWorldNormal;
    vec2 tUV;
    float fHeight;
} tShaderOut;

layout(set = 3, binding = 0) uniform PL_DYNAMIC_DATA
{
    plGpuDynPlanetData tData;
} tDynamicData;

void main()
{
    vec3 worldHigh = inHighPos;
    vec3 worldLow = inLowPos;
    vec3 worldPos = normalize(worldHigh + worldLow) * tDynamicData.tData.fRadius;
    worldHigh = worldPos;
    worldLow = vec3(0.0);

    vec3 t1 = worldLow - tDynamicData.tData.tCameraPosLow.xyz;
    vec3 e = t1 - worldLow;
    vec3 t2 = ((-tDynamicData.tData.tCameraPosLow.xyz - e)
             + (worldLow - (t1 - e)))
             + worldHigh
             - tDynamicData.tData.tCameraPosHigh.xyz;

    vec3 highDifference = t1 + t2;
    vec3 lowDifference = t2 - (highDifference - t1);
    vec3 viewPosition = highDifference + lowDifference;

    gl_Position = tDynamicData.tData.tCameraViewProjection * vec4(viewPosition, 1.0);

    vec3 colors[16];
    float strength = 0.18;
    colors[0] = vec3(strength, 0.0, 0.0);
    colors[1] = vec3(0.0, strength, 0.0);
    colors[2] = vec3(0.0, 0.0, strength);
    colors[3] = vec3(strength, strength, 0.0);
    colors[4] = vec3(strength, 0.0, strength);
    colors[5] = vec3(0.0, strength, strength);
    colors[6] = vec3(strength);
    colors[7] = vec3(strength * 3.0, strength, strength);
    colors[8] = vec3(strength * 2.0, 0.0, 0.0);
    colors[9] = vec3(0.0, strength * 2.0, 0.0);
    colors[10] = vec3(0.0, 0.0, strength * 2.0);
    colors[11] = vec3(strength * 2.0, strength * 2.0, 0.0);
    colors[12] = vec3(strength * 2.0, 0.0, strength * 2.0);
    colors[13] = vec3(0.0, strength, strength * 2.0);
    colors[14] = vec3(strength, strength * 2.0, strength * 2.0);
    colors[15] = vec3(strength * 3.5, strength, strength);

    int colorIndex = bool(tDynamicData.tData.tFlags & PL_TERRAIN_SHADER_FLAGS_SHOW_CHUNKS)
        ? (tDynamicData.tData.iChunkID & 15)
        : (tDynamicData.tData.iLevel & 15);

    tShaderOut.tColor = vec4(colors[colorIndex], 1.0);
    tShaderOut.tWorldPosition = worldPos;
    tShaderOut.tWorldNormal = normalize(inNormal);
    tShaderOut.tUV = inUV;
    tShaderOut.fHeight = 0.0;
}
