#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "pl_shader_interop_planet.h"

layout(location = 0) in struct plShaderIn {
    vec4 tColor;
    vec3 tWorldPosition;
    vec3 tWorldNormal;
    vec2 tUV;
    float fHeight;
} tShaderIn;

layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform PL_DYNAMIC_DATA
{
    plGpuDynPlanetData tData;
} tDynamicData;

void main()
{
    vec3 normal = normalize(tShaderIn.tWorldNormal);
    vec3 lightDirection = normalize(tDynamicData.tData.tLightDirection);

    float lambert = max(0.0, dot(normal, lightDirection));
    float heightT = clamp(tShaderIn.fHeight / 8000.0 + 0.5, 0.0, 1.0);

    vec3 lowColor = vec3(0.31, 0.27, 0.29);
    vec3 midColor = vec3(0.48, 0.48, 0.46);
    vec3 highColor = vec3(0.80, 0.78, 0.72);
    vec3 baseColor = heightT < 0.5
        ? mix(lowColor, midColor, heightT * 2.0)
        : mix(midColor, highColor, (heightT - 0.5) * 2.0);

    vec3 color = baseColor * (0.16 + lambert * 0.94);

    if(bool(tDynamicData.tData.tFlags & PL_TERRAIN_SHADER_FLAGS_SHOW_LEVELS) ||
       bool(tDynamicData.tData.tFlags & PL_TERRAIN_SHADER_FLAGS_SHOW_CHUNKS))
    {
        color += tShaderIn.tColor.rgb;
    }

    if(bool(tDynamicData.tData.tFlags & PL_TERRAIN_SHADER_FLAGS_WIREFRAME))
    {
        color = max(color, tShaderIn.tColor.rgb + vec3(0.35));
    }

    outColor = vec4(color, 1.0);
}
