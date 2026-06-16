#version 450 core

layout(location = 0) in struct {
    vec4 color;
    vec3 normal;
} In;

layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform PL_DYNAMIC_DATA
{
    mat4 tMVP;
    vec4 tColor;
    vec4 tLightDirection;
} tObjectInfo;

void main()
{
    vec3 normal = normalize(In.normal);
    vec3 lightDirection = normalize(tObjectInfo.tLightDirection.xyz);
    float diffuse = max(0.0, dot(normal, lightDirection));
    float lit = 0.18 + 0.82 * diffuse;
    outColor = vec4(In.color.rgb * lit, In.color.a);
}
