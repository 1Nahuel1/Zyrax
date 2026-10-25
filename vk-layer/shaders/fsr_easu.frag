#version 450
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
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

AF4 FsrEasuRF(AF2 p) { return textureGather(srcTex, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(srcTex, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(srcTex, p, 2); }

void main() {
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
    fragColor = vec4(c, 1.0);
}
