#version 450 core
// T-I5a-5 (B3): lightning light-pulse + bolt overlay (RENDER-ONLY, one-way F2).
// A strike is a deterministic SIM world event (in the `weather` world_hash
// sub-hash). This overlay is the full-scene render response for the frame(s) the
// bolt shows: a transient additive luminance PULSE over the whole frame + a
// screen-space BOLT polyline rasterized as a distance-to-line test in NDC. It is
// drawn additively into the (already tonemapped) lighting FBO AFTER the skybox so
// it composites over both terrain and sky -- the photography timing shot.
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D u_scene;   // the composited (lit + sky) tonemapped color

#define MAX_BOLT_POINTS 96
uniform int   u_active = 0;
uniform float u_pulse = 0.0;             // full-scene additive flash strength
uniform vec3  u_color = vec3(0.72, 0.82, 1.0);
uniform vec2  u_strikeNdc = vec2(0.0);   // strike point in NDC (radial centre)
uniform float u_boltWidth = 0.012;       // bolt core half-width (NDC)
uniform float u_boltGlow = 0.040;        // bolt glow falloff radius (NDC)
uniform int   u_boltCount = 0;
uniform vec2  u_bolt[MAX_BOLT_POINTS];   // flattened NDC polyline (x<=-2 == pen-up)
uniform float u_aspect = 1.777;          // framebuffer width/height

// Shortest distance (aspect-corrected NDC) from a screen point to the bolt
// polyline. A pen-up separator (x <= -2.0) breaks disjoint strokes.
float boltDistance(vec2 ndc) {
    float best = 1e9;
    vec2 prev = vec2(0.0);
    bool have_prev = false;
    for (int i = 0; i < MAX_BOLT_POINTS; ++i) {
        if (i >= u_boltCount) break;
        vec2 p = u_bolt[i];
        if (p.x <= -2.0) { have_prev = false; continue; }
        if (have_prev) {
            vec2 a = vec2(prev.x * u_aspect, prev.y);
            vec2 b = vec2(p.x * u_aspect, p.y);
            vec2 q = vec2(ndc.x * u_aspect, ndc.y);
            vec2 ab = b - a;
            float t = clamp(dot(q - a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
            best = min(best, length(q - (a + t * ab)));
        }
        prev = p;
        have_prev = true;
    }
    return best;
}

void main() {
    vec3 color = texture(u_scene, TexCoords).rgb;
    if (u_active == 1 && u_pulse > 0.0) {
        vec2 ndc = TexCoords * 2.0 - 1.0;
        // Full-scene flash: additive lift, mildly stronger toward the strike.
        float radial = 1.0 - 0.35 * clamp(length((ndc - u_strikeNdc) * vec2(u_aspect, 1.0)) / 2.0, 0.0, 1.0);
        color += u_color * (u_pulse * radial);
        // Bolt: bright near-white core + soft glow halo along the polyline.
        float bd = boltDistance(ndc);
        float core = 1.0 - smoothstep(0.0, u_boltWidth, bd);
        float glow = 1.0 - smoothstep(u_boltWidth, u_boltGlow, bd);
        vec3 boltCol = mix(u_color, vec3(1.0), 0.85);
        color += boltCol * (core * 3.0 + glow * 1.0) * max(u_pulse, 1.0);
    }
    FragColor = vec4(color, 1.0);
}
