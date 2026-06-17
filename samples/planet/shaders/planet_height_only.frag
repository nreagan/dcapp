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
    float t = clamp((tShaderIn.fHeight + 8000.0) / 13000.0, 0.0, 1.0);
    vec3 color = mix(vec3(0.18, 0.30, 0.08), vec3(0.95, 0.94, 0.90), t);

    if(bool(tDynamicData.tData.tFlags & PL_TERRAIN_SHADER_FLAGS_SHOW_LEVELS) ||
       bool(tDynamicData.tData.tFlags & PL_TERRAIN_SHADER_FLAGS_SHOW_CHUNKS))
    {
        color += tShaderIn.tColor.rgb;
    }

    outColor = vec4(color, 1.0);
}
