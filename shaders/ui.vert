#version 450
// Nuklear UI vertex shader. Vertex layout: pos(vec2), uv(vec2), color(ubyte4 norm).

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(set = 1, binding = 0) uniform UIUBO {
    mat4 proj;
} u;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV     = inUV;
    vColor  = inColor;
    gl_Position = u.proj * vec4(inPos, 0.0, 1.0);
}
