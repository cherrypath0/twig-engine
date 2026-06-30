#version 450
// 3D mesh vertex shader (SDL3-GPU binding model: vertex uniforms in set 1)

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 1, binding = 0) uniform VertexUBO {
    mat4 mvp;
    mat4 model;
} ubo;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;

void main() {
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
    vNormal     = mat3(ubo.model) * inNormal;
    vUV         = inUV;
    vWorldPos   = (ubo.model * vec4(inPos, 1.0)).xyz;
}
