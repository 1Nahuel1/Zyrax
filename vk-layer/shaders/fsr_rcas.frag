#version 450
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"

layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform PC {
    uvec4 con0;
    uvec4 con1;
    uvec4 con2;
    uvec4 con3;
    uvec4 rcas;
} pc;

AF4 FsrRcasLoadF(ASU2 p) {
    ivec2 q = clamp(ivec2(p), ivec2(0), textureSize(srcTex, 0) - ivec2(1));
    return texelFetch(srcTex, q, 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

void main() {
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), pc.rcas);
    fragColor = vec4(r, g, b, 1.0);
}
