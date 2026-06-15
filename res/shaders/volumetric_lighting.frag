#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

// T-I5a-6: ANALYTIC aerial-perspective (distance fog) term.
//
// This previously-dormant shader is now wired as the aerial-perspective pass
// (RenderPipeline::execute_aerial_pass). It is a CHEAP per-pixel ANALYTIC term
// (no raymarch loop, no froxel volume -- critique F6 scope stop): a single
// exponential extinction over view distance whose in-scatter color comes from
// the SAME sky scattering as the dome (the sky-view + transmittance LUTs), so
// distance fog, sky, sun and ambient share one transmittance and the low-sun
// palette stays coherent. Output composites the aerial haze OVER the lit scene
// via standard SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending (alpha = fog).

uniform sampler2D gDepth;      // scene depth

// Sky scattering LUTs (shared with the skybox dome).
uniform sampler2D u_skyViewLut;        // 192x108 sky dome radiance
uniform sampler2D u_transmittanceLut;  // 256x64 transmittance

uniform mat4 u_inverseView;
uniform mat4 u_inverseProjection;
uniform vec3 u_viewPos;
uniform vec3 u_sunDirection;   // TOWARD-sun unit vector (sun disc convention)
uniform float u_sunCosZenith;  // dot(sunDir, up)
uniform float u_skyDayFactor;  // night-darkening envelope (matches the dome)

// Aerial-perspective tuning. Distance at which fog reaches ~63% opacity scales
// inversely with this; kept modest so near terrain stays clear and only the
// far field hazes (the FarLodHorizon sky-ratio premise is unaffected).
uniform float u_aerialDensity = 0.0016;
uniform float u_aerialMaxDistance = 1600.0;

const float PI = 3.14159265359;

vec3 worldPositionFromDepth(vec2 uv, float depth) {
    float z = depth * 2.0 - 1.0;
    vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 viewSpace = u_inverseProjection * clip;
    viewSpace /= viewSpace.w;
    vec4 world = u_inverseView * viewSpace;
    return world.xyz;
}

// Sample the sky-view LUT in the view direction so the in-scatter color matches
// the warm low-sun horizon the dome shows.
vec3 sampleSkyInscatter(vec3 viewDir) {
    float cosV = clamp(viewDir.y, -1.0, 1.0);
    float zenith = acos(cosV);              // 0 = up, pi = down
    vec2 vh = normalize(vec2(viewDir.x, viewDir.z) + 1e-5);
    vec2 sh = normalize(vec2(u_sunDirection.x, u_sunDirection.z) + 1e-5);
    float az = acos(clamp(dot(vh, sh), -1.0, 1.0));   // [0, pi]
    float u = az / (2.0 * PI);
    float v = clamp(zenith / PI, 0.0, 1.0);
    return texture(u_skyViewLut, vec2(u, v)).rgb;
}

// Transmittance toward the sun (ground viewer), the warm aerial hue shared with
// the skybox dome grade.
vec3 sunTransmittance(float cosZenith) {
    float u = clamp((cosZenith + 1.0) * 0.5, 0.0, 1.0);
    return texture(u_transmittanceLut, vec2(u, 0.0)).rgb;
}

void main() {
    float sceneDepth = texture(gDepth, TexCoords).r;
    // Far-depth (sky) pixels: the dome + its own scattering already supply the
    // color. Leave them untouched (alpha 0) so the horizon sky-ratio that
    // FarLodHorizon measures does not move.
    if (sceneDepth >= 0.9999) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec3 worldPos = worldPositionFromDepth(TexCoords, sceneDepth);
    vec3 toFrag = worldPos - u_viewPos;
    float dist = min(length(toFrag), u_aerialMaxDistance);
    vec3 viewDir = normalize(toFrag);

    // Analytic exponential fog opacity over distance.
    float fog = 1.0 - exp(-dist * u_aerialDensity);
    fog = clamp(fog, 0.0, 1.0);

    // In-scatter color from the shared sky scattering, scaled into the HDR
    // display range and gated by the night envelope so distance fog vanishes at
    // night exactly as the dome darkens.
    vec3 inscatter = sampleSkyInscatter(viewDir) * 60.0;
    inscatter *= clamp(u_skyDayFactor, 0.0, 1.0);

    // T-I5a-6 FIX (FarLodHorizon): the raw sky-view in-scatter is BLUE-dominant
    // (b > r). Composited over the far-LOD terrain at the live/far seam it tinted
    // the distant ground blue enough to trip the FarLodHorizon boundary-band sky
    // detector (b >= r+35, g >= r+18 reads as "sky") -- the aerial term was
    // breaking the very horizon sky-ratio it promised not to touch. We warm the
    // aerial in-scatter toward the sun-path transmittance hue (the SAME warm grade
    // the dome uses, so the palette stays coherent) and pull its blue down toward
    // green, so the far-terrain haze is a pale/warm aerial veil rather than blue
    // sky -- it no longer classifies as a sky band over the terrain.
    vec3 aerialTrans = sunTransmittance(u_sunCosZenith);
    float aNorm = max(aerialTrans.r, max(aerialTrans.g, aerialTrans.b));
    vec3 aHue = pow(clamp(aerialTrans / max(aNorm, 1e-4), vec3(0.0), vec3(1.0)),
                    vec3(1.0, 1.4, 2.4));
    float aLuma = max(dot(aHue, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    inscatter *= aHue / aLuma;               // warm hue, luminance preserved
    inscatter.b = min(inscatter.b, inscatter.g);   // never blue-dominant over land

    // alpha = fog composites the aerial haze OVER the lit terrain.
    FragColor = vec4(inscatter, fog);
}
