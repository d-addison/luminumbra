#version 450 core
// lightning light-pulse + bolt overlay (, one-way ).
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
// GROUND-IMPACT flash. The bolt must visibly TOUCH DOWN:
// a bright radial bloom at the touchdown point sells the strike connecting to the
// terrain. u_groundNdc is the projected ground terminus; u_groundFlash scales it.
uniform vec2  u_groundNdc = vec2(0.0, -1.0);
uniform float u_groundFlash = 0.0;       // 0 = no impact bloom
// DARK STORM CLOUD the bolt emerges from. u_cloudNdc is
// the bolt-top anchor (cloud base) in NDC; u_cloudDark scales a dark, billowing
// cloud mass painted across the upper frame around that anchor. The flash then
// lights this cloud from within so the strike clearly STEMS FROM the cloud.
uniform vec2  u_cloudNdc = vec2(0.0, 0.85);
uniform float u_cloudDark = 0.0;         // 0 = no cloud overlay

// --- cheap value-noise FBM for the cloud silhouette (hash-based, no textures) ---
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i + vec2(0.0, 0.0));
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += amp * vnoise(p);
        p *= 2.02;
        amp *= 0.5;
    }
    return v;
}

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

        // --- DARK STORM CLOUD ------------------------
        // Paint a dark, billowing cloud mass across the upper frame, densest around
        // the bolt-top anchor (u_cloudNdc) and thinning downward, so the bolt
        // emerges from a visible cloud rather than thin air. The cloud is composited
        // FIRST (darkening the sky) and then lit by the flash below.
        float cloudLight = 0.0; // remembered cloud density at this pixel for the flash
        if (u_cloudDark > 0.0) {
            // Cloud field: low-frequency FBM in aspect-corrected NDC, biased so the
            // billows pool near the top of the frame and around the strike column.
            vec2 cp = vec2(ndc.x * u_aspect, ndc.y);
            float billow = fbm(cp * 2.4 + vec2(7.0, 3.0));
            billow = billow * 0.65 + 0.5 * fbm(cp * 5.0 + vec2(19.0, 2.0));
            // Vertical envelope: full strength at/above the cloud base anchor,
            // tapering off below it (the underside of the storm deck).
            float vert = smoothstep(u_cloudNdc.y - 0.85, u_cloudNdc.y + 0.15, ndc.y);
            // Horizontal pooling toward the strike column (denser overhead the bolt).
            float horiz = 1.0 - 0.45 * clamp(abs(ndc.x - u_cloudNdc.x) * u_aspect / 1.8, 0.0, 1.0);
            float density = clamp(billow * vert * horiz, 0.0, 1.0);
            // Soft cloud coverage mask (rolling billow edges, not a flat band).
            float cover = smoothstep(0.42, 0.78, density);
            cloudLight = cover;
            // Darken the scene under the cloud toward a deep storm-grey.
            vec3 cloudCol = vec3(0.06, 0.07, 0.10);
            color = mix(color, cloudCol, cover * u_cloudDark);
        }

        // Full-scene flash: additive lift, mildly stronger toward the strike.
        float radial = 1.0 - 0.35 * clamp(length((ndc - u_strikeNdc) * vec2(u_aspect, 1.0)) / 2.0, 0.0, 1.0);
        color += u_color * (u_pulse * radial);
        // Flash lights the storm cloud FROM WITHIN: the strike briefly back-lights
        // the dark deck near the bolt top, the readable "thunderhead lit by lightning"
        // signature. Brightest in the cloud directly around the strike column.
        if (u_cloudDark > 0.0 && cloudLight > 0.0) {
            float toStrike = 1.0 - clamp(length((ndc - u_cloudNdc) * vec2(u_aspect, 1.0)) / 1.6, 0.0, 1.0);
            vec3 litCloud = mix(u_color, vec3(1.0), 0.4);
            color += litCloud * cloudLight * toStrike * u_pulse * 1.6;
        }
        // Bolt: a THIN hot near-white core with a soft, falling-off bluish glow
        // halo along the polyline. The old single
        // wide smoothstep + core*3.0 painted a fat opaque white worm; this splits
        // the response into (1) a hard, narrow hot core only a couple px wide that
        // reads as the bright channel, and (2) an additive glow that decays
        // smoothly with distance so the bolt has a luminous halo rather than a
        // hard-edged blob. The core half-width is clamped well below the glow
        // radius so the structure stays thin regardless of the uniform tuning.
        // a THIN, SHARP, near-white filament. The owner
        // saw a "fat worm" -- so the core is pinned to a hard ~1-2px ribbon (a
        // near-binary edge a hair wide in NDC) regardless of the glow tuning, and
        // the glow halo is kept tight + faint so it frames the bolt rather than
        // bloating it. The result reads as a hot jagged crack of light, not a tube.
        float bd = boltDistance(ndc);
        // Core half-width in NDC clamped to a ~1-2px equivalent ribbon so the bolt
        // is sharp at any resolution. (0.0016 NDC ~= 1.5px on a 1080-tall frame.)
        float coreHalf = clamp(min(u_boltWidth * 0.18, u_boltGlow * 0.10), 0.0010, 0.0024);
        // Hot core: hard near-binary inner ribbon (thin bright filament). A tiny
        // smoothstep band gives 1px antialiasing without widening the core.
        float core = 1.0 - smoothstep(coreHalf * 0.6, coreHalf, bd);
        // Glow: tight quadratic falloff, kept faint so it does not read as girth.
        float glowRadius = min(u_boltGlow, 0.030);
        float glowLin = 1.0 - smoothstep(coreHalf, glowRadius, bd);
        float glow = glowLin * glowLin;
        vec3 hotCore = mix(u_color, vec3(1.0), 0.97);   // near-white hot filament
        vec3 glowCol = u_color;                          // bluish additive halo
        // Core is the dominant bright channel; glow is a thin translucent halo.
        color += hotCore * (core * 3.2) * max(u_pulse, 1.0);
        color += glowCol * (glow * 0.40) * max(u_pulse, 1.0);

        // GROUND-IMPACT bloom -- a bright flash at the touchdown point so the bolt
        // visibly CONNECTS to the terrain and lights the ground it strikes.
        // the bloom is now anchored at the REAL
        // projected touchdown (the host only enables it when that point is on-screen),
        // and it is FLATTENED vertically + tightened so it reads as ground illumination
        // spreading along the surface at the strike, not a hovering circular saucer.
        // A small hot core sits at the contact point; a wider, low, horizontally-biased
        // wash lifts the ground around it.
        // TAME THE IMPACT BLOOM. The old bloom was a blown-out
        // white smear that floated/bled across the water -- because (a) its radii were
        // large (core to 0.10, wash to 0.26 NDC) so it spread a big disc, (b) the wash
        // strength (0.30) was high enough to wash a wide area to white over the bright
        // water, and (c) the near-white core (mix 0.7) clipped hot. Now it is a small,
        // dim, tightly-localized contact glow: a tiny hot core hugging the strike point
        // and a much smaller, fainter, vertically-squashed wash so it lights the surface
        // immediately at the touchdown WITHOUT smearing across the water.
        if (u_groundFlash > 0.0) {
            vec2 d = (ndc - u_groundNdc) * vec2(u_aspect, 1.0);
            // Vertical squash: the glow hugs the ground line (wider than it is tall),
            // so it does not read as a free-floating round disc.
            vec2 dFlat = vec2(d.x, d.y * 2.6);
            float gd = length(dFlat);
            // Tight hot contact core right at the touchdown (smaller than before).
            float core = 1.0 - smoothstep(0.0, 0.055, gd);
            core = core * core;
            // Low, narrow ground wash -- spreads only a short way around the contact,
            // kept faint so it lifts the surface rather than painting a bright ellipse.
            float wash = 1.0 - smoothstep(0.0, 0.14, gd);
            wash = wash * wash;
            // Cooler, less-white core so it does not clip to a white smear on water.
            vec3 coreCol = mix(u_color, vec3(1.0), 0.45);
            vec3 washCol = u_color;
            color += coreCol * core * u_groundFlash * 0.65;
            color += washCol * wash * u_groundFlash * 0.16;
        }
    }
    FragColor = vec4(color, 1.0);
}
