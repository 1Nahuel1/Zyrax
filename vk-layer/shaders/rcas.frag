#version 450

// Zyrax FSR-style RCAS (Robust Contrast-Adaptive Sharpening) fragment pass.
// Inspired by AMD FidelityFX FSR 1.0 RCAS. Reads the presented frame from a
// scratch copy, sharpens it with a contrast-adaptive kernel and writes the
// result straight into the swapchain image used as the color attachment.

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform PushConstants {
    vec2 resolution;
    float sharpness;
    float pad;
} pc;

vec3 fetch(ivec2 p) {
    ivec2 hi = ivec2(pc.resolution) - ivec2(1);
    return texelFetch(srcTex, clamp(p, ivec2(0), hi), 0).rgb;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);

    vec3 c  = fetch(p);
    vec3 n  = fetch(p + ivec2( 0, -1));
    vec3 s  = fetch(p + ivec2( 0,  1));
    vec3 e  = fetch(p + ivec2( 1,  0));
    vec3 w  = fetch(p + ivec2(-1,  0));
    vec3 nw = fetch(p + ivec2(-1, -1));
    vec3 ne = fetch(p + ivec2( 1, -1));
    vec3 sw = fetch(p + ivec2(-1,  1));
    vec3 se = fetch(p + ivec2( 1,  1));

    vec3 blur = (n + s + e + w + nw + ne + sw + se) * 0.125;

    // Local min/max clamp prevents halos (RCAS behaviour).
    vec3 mn = min(c, min(min(n, s), min(e, w)));
    vec3 mx = max(c, max(max(n, s), max(e, w)));

    float amount = 0.5 + 2.0 * clamp(pc.sharpness, 0.0, 1.0);
    vec3 sharp = c + (c - blur) * amount;
    vec3 outc = clamp(sharp, mn, mx);

    fragColor = vec4(outc, 1.0);
}
