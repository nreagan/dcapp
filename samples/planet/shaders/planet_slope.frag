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

    vec3 radial = normalize(tShaderIn.tWorldPosition);
    float slopeDeg = degrees(acos(clamp(dot(normal, radial), 0.0, 1.0)));

    vec3 cFlat = vec3(0.55, 0.52, 0.48);
    vec3 cModerate = vec3(0.85, 0.62, 0.15);
    vec3 cSteep = vec3(0.78, 0.12, 0.10);

    vec3 color = cFlat;
    color = mix(color, cModerate, step(5.0, slopeDeg));
    color = mix(color, cSteep, step(10.0, slopeDeg));
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
