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
    uvec4 ui;
    uvec4 ui2;
} pc;

AF4 FsrRcasLoadF(ASU2 p) {
    ivec2 q = clamp(ivec2(p), ivec2(0), textureSize(srcTex, 0) - ivec2(1));
    return texelFetch(srcTex, q, 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

// 5x7 bitmap font, one uint per row (bits 4..0).
uint glyphRow(int ch, int row) {
    if (ch == 48) { uint r[7] = uint[7](14u,17u,19u,21u,25u,17u,14u); return r[row]; }
    if (ch == 49) { uint r[7] = uint[7](4u,12u,4u,4u,4u,4u,14u); return r[row]; }
    if (ch == 50) { uint r[7] = uint[7](14u,17u,1u,2u,4u,8u,31u); return r[row]; }
    if (ch == 51) { uint r[7] = uint[7](31u,2u,4u,2u,1u,17u,14u); return r[row]; }
    if (ch == 52) { uint r[7] = uint[7](2u,6u,10u,18u,31u,2u,2u); return r[row]; }
    if (ch == 53) { uint r[7] = uint[7](31u,16u,30u,1u,1u,17u,14u); return r[row]; }
    if (ch == 54) { uint r[7] = uint[7](6u,8u,16u,30u,17u,17u,14u); return r[row]; }
    if (ch == 55) { uint r[7] = uint[7](31u,1u,2u,4u,8u,8u,8u); return r[row]; }
    if (ch == 56) { uint r[7] = uint[7](14u,17u,17u,14u,17u,17u,14u); return r[row]; }
    if (ch == 57) { uint r[7] = uint[7](14u,17u,17u,15u,1u,2u,12u); return r[row]; }
    if (ch == 70) { uint r[7] = uint[7](31u,16u,16u,30u,16u,16u,16u); return r[row]; } // F
    if (ch == 80) { uint r[7] = uint[7](30u,17u,17u,30u,16u,16u,16u); return r[row]; } // P
    if (ch == 83) { uint r[7] = uint[7](15u,16u,16u,14u,1u,1u,30u); return r[row]; } // S
    if (ch == 67) { uint r[7] = uint[7](14u,17u,16u,16u,16u,17u,14u); return r[row]; } // C
    if (ch == 76) { uint r[7] = uint[7](16u,16u,16u,16u,16u,16u,31u); return r[row]; } // L
    return 0u;
}

void main() {
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), pc.rcas);

    vec3 col = vec3(r, g, b);
    uint flags = pc.ui.w;
    bool showFps = (flags & 1u) != 0u;
    bool showScale = (flags & 2u) != 0u;
    if (pc.ui.x != 0u && (showFps || showScale)) {
        int chars[15];
        int count = 0;
        int f = min(int(pc.ui.y), 999);
        int s = min(int(pc.ui.z), 999);

        if (showFps) {
            chars[count++] = 70; chars[count++] = 80; chars[count++] = 83; chars[count++] = 32; // "FPS "
            chars[count++] = 48 + (f / 100) % 10;
            chars[count++] = 48 + (f / 10) % 10;
            chars[count++] = 48 + f % 10;
        }
        if (showScale) {
            if (showFps) chars[count++] = 32;
            chars[count++] = 83; chars[count++] = 67; chars[count++] = 76; chars[count++] = 32; // "SCL "
            chars[count++] = 48 + (s / 100) % 10;
            chars[count++] = 48 + (s / 10) % 10;
            chars[count++] = 48 + s % 10;
        }

        float px = 3.0;
        float cellW = 5.0 * px + 3.0;
        float textW = float(count) * cellW - 3.0;
        float textH = 7.0 * px;
        vec2 size = vec2(float(pc.ui2.x), float(pc.ui2.y));
        float margin = 12.0;
        int pos = int((flags >> 2u) & 3u);
        vec2 origin = vec2(margin, margin);
        if (pos == 1) origin = vec2(margin, size.y - textH - margin);
        else if (pos == 2) origin = vec2(size.x - textW - margin, margin);
        else if (pos == 3) origin = vec2(size.x - textW - margin, size.y - textH - margin);

        vec2 q = gl_FragCoord.xy - origin;
        if (q.x >= 0.0 && q.y >= 0.0) {
            int idx = int(floor(q.x / cellW));
            if (idx < count) {
                float lx = q.x - float(idx) * cellW;
                if (lx < 5.0 * px) {
                    int gx = int(floor(lx / px));
                    int gy = int(floor(q.y / px));
                    if (gy < 7) {
                        uint bits = glyphRow(chars[idx], gy);
                        if (((bits >> uint(4 - gx)) & 1u) != 0u) {
                            col = mix(col, vec3(0.0, 1.0, 0.75), 0.85);
                        }
                    }
                }
            }
        }
    }

    fragColor = vec4(col, 1.0);
}
