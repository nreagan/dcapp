#version 450 core

layout(set = 3, binding = 0) uniform PL_DYNAMIC_DATA
{
    mat4 tMVP;
    vec4 tColor;
    vec4 tLightDirection;
} tObjectInfo;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out struct {
    vec4 color;
    vec3 normal;
} Out;

void main()
{
    Out.color = tObjectInfo.tColor;
    Out.normal = normalize(inNormal);
    gl_Position = tObjectInfo.tMVP * vec4(inPos, 1.0);
}
