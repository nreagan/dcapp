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
    float diffuse = max(0.0, dot(normal, lightDirection));
    float ambient = 0.15;

    float t = clamp((tShaderIn.fHeight + 8000.0) / 13000.0, 0.0, 1.0);

    vec3 c0 = vec3(0.18, 0.30, 0.08);
    vec3 c1 = vec3(0.42, 0.52, 0.18);
    vec3 c2 = vec3(0.76, 0.65, 0.22);
    vec3 c3 = vec3(0.72, 0.40, 0.14);
    vec3 c4 = vec3(0.45, 0.30, 0.18);
    vec3 c5 = vec3(0.95, 0.94, 0.90);

    vec3 color = c0;
    color = mix(color, c1, clamp(t / 0.20, 0.0, 1.0));
    color = mix(color, c2, clamp((t - 0.20) / 0.20, 0.0, 1.0));
    color = mix(color, c3, clamp((t - 0.40) / 0.20, 0.0, 1.0));
    color = mix(color, c4, clamp((t - 0.60) / 0.15, 0.0, 1.0));
    color = mix(color, c5, clamp((t - 0.75) / 0.25, 0.0, 1.0));

    color *= diffuse + ambient;

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
