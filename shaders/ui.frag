#version 450
// Nuklear UI fragment shader: tint * font/atlas texture.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D atlas;

void main() {
    outColor = vColor * texture(atlas, vUV);
}
