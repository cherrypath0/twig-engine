#version 450
// Selection mask: render the selected object as solid white into an offscreen
// texture. A later full-screen pass edge-detects this to draw the outline.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(1.0); }
