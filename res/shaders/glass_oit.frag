#version 450 core
// the WBOIT glass accumulation. Each pane writes its
// premultiplied color weighted by the McGuire-Bavoil depth weight (OitModel.h -
// the GLSL mirror) into ACCUM (blend ONE/ONE) and its coverage into REVEAL
// (blend ZERO/ONE_MINUS_SRC_ALPHA -> reveal = product of (1 - a_i)).
//
// Glass color = the REFRACTED opaque scene (screen-space offset along the
// surface normal - ; the same pre-water snapshot WaterPass refracts,
// so glass does not refract water - documented approximation) filtered by the
// pane's Beer-Lambert transmission (GlassTintModel.h: T(d) = tint^d), plus a
// small fresnel edge highlight.
layout (location = 0) out vec4 outAccum;
layout (location = 1) out vec4 outReveal;

in vec3 vWorldPos;
in vec3 vWorldNormal;
in float vViewDepth;

uniform sampler2D u_opaqueScene; // lighting.opaque_color (the pre-water snapshot)
uniform vec2 u_screenSize;
uniform vec3 u_cameraPos;
uniform vec3 u_tint;       // unit-thickness transmission (GlassTintModel)
uniform float u_thickness;
uniform float u_refractionStrength; // screen-space offset scale (pixels-ish)

void main() {
    vec3 T = pow(clamp(u_tint, 0.0, 1.0), vec3(max(u_thickness, 0.0)));

    // Screen-space refraction: offset the background lookup along the projected
    // surface normal, scaled down with distance so far panes distort less.
    vec2 uv = gl_FragCoord.xy / u_screenSize;
    vec3 V = normalize(vWorldPos - u_cameraPos);
    vec3 N = normalize(vWorldNormal);
    if (dot(N, V) > 0.0) { N = -N; } // double-sided pane: face the viewer
    vec2 offset = N.xy * u_refractionStrength / max(vViewDepth, 1.0);
    vec3 refracted = texture(u_opaqueScene, clamp(uv + offset, vec2(0.001), vec2(0.999))).rgb;

    // Fresnel-ish edge highlight (Schlick, F0 = 0.04 glass).
    float fresnel = 0.04 + 0.96 * pow(1.0 - abs(dot(N, -V)), 5.0);

    vec3 color = refracted * T + vec3(fresnel) * 0.35;
    // Coverage: tinted glass mostly REPLACES the un-refracted background behind
    // it (the refracted+filtered scene is the new content).
    float alpha = 0.85;

    // McGuire-Bavoil depth weight - mirrors OitModel::DepthWeight exactly.
    float d = max(vViewDepth, 1e-2) / 200.0;
    float w = clamp(0.03 / (1e-5 + d * d * d * d), 1e-2, 3e3) * alpha;

    outAccum = vec4(color * alpha * w, alpha * w);
    outReveal = vec4(0.0, 0.0, 0.0, alpha); // ZERO/ONE_MINUS_SRC_ALPHA -> product
}
