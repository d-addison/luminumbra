#version 450 core
// Composites the projected world-space bolt and localized lightning response
// after terrain and sky. Cloud coverage comes from the world cloud renderer.
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D u_sceneDepth; // D32F reversed-Z: near 1, sky 0
uniform mat4 u_gbufferInverseProjection; // selected Legacy or Ladder projection
uniform vec2 u_jitter_ndc; // current G-buffer projection jitter; zero when TAAU is off
uniform sampler2D u_scene;   // the composited (lit + sky) tonemapped color

#define MAX_BOLT_POINTS 96
#define MAX_STRIKE_FLASHES 8
// Matches Rendering::kLiveBoltPenUp and IsLiveBoltPenUp in LiveStrikeFlash.h.
// Bolt endpoints are clipped to the view with a glow margin before upload.
const vec2 kLiveBoltPenUp = vec2(-3.0, -3.0);
uniform int   u_active = 0;
uniform float u_pulse = 0.0;             // strongest pulse: overlay gate and bolt brightness
uniform vec3  u_color = vec3(0.72, 0.82, 1.0);
uniform int   u_flashCount = 0;
uniform vec2  u_flashNdc[MAX_STRIKE_FLASHES];
uniform float u_flashPulse[MAX_STRIKE_FLASHES];
uniform int   u_flashOnscreen[MAX_STRIKE_FLASHES];
uniform float u_boltWidth = 0.012;       // bolt core half-width (NDC)
uniform float u_boltGlow = 0.040;        // bolt glow falloff radius (NDC)
uniform int   u_boltCount = 0;
uniform vec2  u_bolt[MAX_BOLT_POINTS];   // flattened NDC polyline with kLiveBoltPenUp breaks
uniform float u_boltInverseDepth[MAX_BOLT_POINTS]; // reciprocal positive view depth
uniform float u_aspect = 1.777;          // framebuffer width/height
// GROUND-IMPACT flash. The bolt must visibly TOUCH DOWN:
// a bright radial bloom at the touchdown point sells the strike connecting to the
// terrain. u_groundNdc is the projected ground terminus; u_groundFlash scales it.
uniform vec2  u_groundNdc = vec2(0.0, -1.0);
uniform float u_groundFlash = 0.0;       // 0 = no impact bloom
// A finite flash footprint in aspect-corrected NDC. Scene luminance limits the
// response in dark exposures; black pixels receive no ambient visibility floor.
float flashResponse(float distanceToStrike, float sceneLuma) {
    return (1.0 - smoothstep(0.0, 1.6, distanceToStrike)) * clamp(sceneLuma * 4.0, 0.0, 1.0);
}

// Only offscreen strikes receive a bounded scene-wide floor, including the
// kLiveBoltPenUp sentinel used for strikes behind the camera. strikeExtent is the larger
// absolute NDC component of the centre; the floor ramps in smoothly past the edge.
float offscreenFlashFloor(float strikeExtent, float sceneLuma) {
    return 0.35 * smoothstep(1.0, 1.3, strikeExtent) * clamp(sceneLuma * 4.0, 0.0, 1.0);
}

// Reconstruct positive reciprocal view depth in the scene's selected projection.
// Clear sky and invalid samples never occlude. This also handles Ladder's far
// plane and temporal jitter without assuming the bolt used the same projection.
float sceneInverseDepth(vec2 uv) {
    // projection[2].xy += jitter shifts raster NDC by minus jitter (w = -z).
    // The overlay is unjittered: sample its ray in the jittered G-buffer, and
    // use that same NDC with the inverse of the jittered projection.
    vec2 depthUv = uv - u_jitter_ndc * 0.5;
    float depth = texture(u_sceneDepth, depthUv).r;
    if (isnan(depth) || isinf(depth) || depth <= 0.0 || depth > 1.0) return 0.0;
    vec4 view = u_gbufferInverseProjection * vec4(depthUv * 2.0 - 1.0, depth, 1.0);
    if (any(isnan(view)) || any(isinf(view)) || view.z >= 0.0 || view.w <= 0.0) return 0.0;
    float inverseDepth = view.w / -view.z;
    return isnan(inverseDepth) || isinf(inverseDepth) ? 0.0 : inverseDepth;
}

float boltVisibility(float boltDepth, float sceneDepth) {
    if (sceneDepth <= 0.0) return 1.0;
    // No attenuation in front or at equal depth. Fade over one percent of the
    // occluder's distance to avoid a hard depth seam at a nearly touching ridge.
    return 1.0 - smoothstep(0.0, 0.01, (sceneDepth - boltDepth) / sceneDepth);
}

vec2 channelResponse(float distance, float coreHalf, float glowRadius) {
    float core = 1.0 - smoothstep(coreHalf * 0.6, coreHalf, distance);
    float glowLin = glowRadius > coreHalf ? 1.0 - smoothstep(coreHalf, glowRadius, distance) : 0.0;
    return vec2(core, glowLin * glowLin);
}

// Compare each segment independently before taking the strongest contribution:
// a hidden branch must not mask another visible channel at the same pixel.
vec2 boltResponse(vec2 ndc) {
    float coreHalf = clamp(min(u_boltWidth * 0.18, u_boltGlow * 0.10), 0.0010, 0.0024);
    float glowRadius = min(u_boltGlow, 0.030);
    float sceneDepth = sceneInverseDepth(TexCoords);
    float best = 1e9;
    vec2 faded = vec2(0.0);
    vec2 prev = vec2(0.0);
    float prevDepth = 0.0;
    bool have_prev = false;
    for (int i = 0; i < MAX_BOLT_POINTS; ++i) {
        if (i >= u_boltCount) break;
        vec2 p = u_bolt[i];
        float depth = u_boltInverseDepth[i];
        if (all(equal(p, kLiveBoltPenUp)) || isnan(depth) || isinf(depth) || depth <= 0.0) {
            have_prev = false;
            continue;
        }
        if (have_prev) {
            vec2 a = vec2(prev.x * u_aspect, prev.y);
            vec2 b = vec2(p.x * u_aspect, p.y);
            vec2 q = vec2(ndc.x * u_aspect, ndc.y);
            vec2 ab = b - a;
            float lengthSquared = dot(ab, ab);
            float t = clamp(dot(q - a, ab) / max(lengthSquared, 1e-8), 0.0, 1.0);
            float bd = length(q - (a + t * ab));
            // Outside the core/glow support there is no channel contribution.
            if (bd <= max(coreHalf, glowRadius)) {
                // A subpixel stroke can collapse along the view ray. Its nearer
                // endpoint must remain visible regardless of polyline order.
                float boltDepth = lengthSquared > 1e-8 ? mix(prevDepth, depth, t) : max(prevDepth, depth);
                float visibility = boltVisibility(boltDepth, sceneDepth);
                if (visibility == 1.0) {
                    best = min(best, bd);
                } else if (visibility > 0.0) {
                    faded = max(faded, channelResponse(bd, coreHalf, glowRadius) * visibility);
                }
            }
        }
        prev = p;
        prevDepth = depth;
        have_prev = true;
    }
    return max(channelResponse(best, coreHalf, glowRadius), faded);
}

void main() {
    vec3 color = texture(u_scene, TexCoords).rgb;
    if (u_active == 1 && u_pulse > 0.0) {
        vec2 ndc = TexCoords * 2.0 - 1.0;

        float sceneLuma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float flash = 0.0;
        for (int i = 0; i < MAX_STRIKE_FLASHES; ++i) {
            if (i >= u_flashCount) break;
            vec2 centre = u_flashNdc[i];
            float response = 0.0;
            if (!all(equal(centre, kLiveBoltPenUp))) {
                float distanceToStrike = length((ndc - centre) * vec2(u_aspect, 1.0));
                response = flashResponse(distanceToStrike, sceneLuma);
            }
            if (u_flashOnscreen[i] == 0) {
                float strikeExtent = max(abs(centre.x), abs(centre.y));
                response = max(response, offscreenFlashFloor(strikeExtent, sceneLuma));
            }
            flash = max(flash, u_flashPulse[i] * response);
        }
        color += u_color * flash;
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
        vec2 bolt = boltResponse(ndc);
        float core = bolt.x;
        float glow = bolt.y;
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
