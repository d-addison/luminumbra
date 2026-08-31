#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

// ANALYTIC aerial-perspective (distance fog) term.
//
// This previously-dormant shader is now wired as the aerial-perspective pass
// (RenderPipeline::execute_aerial_pass). It is a CHEAP per-pixel ANALYTIC term
// (no raymarch loop, no froxel volume -- regression contract scope stop): a single
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
//  controllable atmosphere: HDR scale of the sky in-scatter veil, and a
// warmth blend (0 = raw sky-view hue -> bluer/crisp aerial; 1 = warmed land
// veil with b clamped <= g -> warm hazy depth without blue-tinting ground).
uniform float u_inscatterStrength = 60.0;
uniform float u_atmosphereWarmth = 1.0;
// VALLEY / GROUND FOG: a ground-hugging mist layer that pools in low terrain and
// fades with altitude, so valleys fill with morning/evening mist while hilltops
// stay clear. Defaulted (C++ need not set them); stronger at low sun (dawn/dusk).
uniform float u_groundFogDensity = 0.022; // accumulation per metre of view depth
uniform float u_fogHeight = 70.0;         // world Y where the mist top fades out
uniform float u_fogThickness = 52.0;      // metres of vertical falloff to the top

// UNDERWATER: when the camera is submerged this pass becomes a murky-water volume
// instead of atmospheric haze — a blue-green tint thickening with view distance
// (limited visibility), applied over EVERYTHING including the surface/sky above.
uniform float u_underwater = 0.0;             // 1.0 when the camera is below a water surface
uniform vec3  u_underwaterTint = vec3(0.04, 0.18, 0.26);
uniform float u_underwaterVisibility = 26.0;  // metres to near-full murk

//  froxel volumetrics: the integrated froxel media volume — rgb = the
// accumulated in-scatter (HDR-linear) in front of a given depth, a = the
// transmittance to it. Mode 0 (default) skips the sampling: byte-identical to
// the pre-froxel analytic-only render. Slice mapping mirrors FroxelGrid.h.
uniform int u_volumetricMode = 0;
uniform sampler3D u_froxelIntegrated;
const float FROXEL_NEAR = 0.5;
const float FROXEL_FAR = 160.0;

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

    // UNDERWATER volume: fog EVERYTHING (scene + the surface/sky above) with a
    // blue-green murk that thickens with view distance, so submerging reads as
    // being underwater (limited visibility) rather than dry air with a tint.
    if (u_underwater > 0.5) {
        float d;
        if (sceneDepth >= 0.9999) {
            d = u_underwaterVisibility * 4.0; // far/surface -> deep murk
        } else {
            vec3 wp = worldPositionFromDepth(TexCoords, sceneDepth);
            d = length(wp - u_viewPos);
        }
        float murk = 1.0 - exp(-d / max(1.0, u_underwaterVisibility));
        FragColor = vec4(u_underwaterTint, clamp(murk, 0.0, 0.92));
        return;
    }

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

    // VALLEY / GROUND FOG: density rises toward the valley floor (low worldPos.y),
    // fades out above u_fogHeight, accumulates over view distance, and is strongest
    // at low sun (dawn/dusk morning mist). Hilltops above the fog top stay clear.
    float heightF = clamp((u_fogHeight - worldPos.y) / max(u_fogThickness, 1.0), 0.0, 1.0);
    heightF *= heightF; // denser toward the valley floor
    // Mist is a DAWN/DUSK phenomenon: zero at high sun (midday has no valley fog),
    // ramping to full only as the sun nears/drops below the horizon. (Also stops the
    // pale midday mist reading as "sky below the horizon" in the PlayerView gate.)
    float lowSunMist = 1.0 - smoothstep(0.08, 0.42, u_sunCosZenith);
    float groundFog = (1.0 - exp(-dist * u_groundFogDensity * heightF)) * lowSunMist;
    groundFog *= clamp(u_skyDayFactor, 0.0, 1.0); // vanish at true night like the dome
    fog = max(fog, clamp(groundFog, 0.0, 0.9));

    // In-scatter color from the shared sky scattering, scaled into the HDR
    // display range and gated by the night envelope so distance fog vanishes at
    // night exactly as the dome darkens.
    vec3 inscatter = sampleSkyInscatter(viewDir) * u_inscatterStrength;
    inscatter *= clamp(u_skyDayFactor, 0.0, 1.0);
    vec3 rawInscatter = inscatter; // un-warmed sky-view hue (crisp/aerial blue)

    //  FIX (FarLodHorizon): the raw sky-view in-scatter is BLUE-dominant
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
    inscatter.b = min(inscatter.b, inscatter.g);   // warmed: never blue-dominant

    //  controllable atmosphere: blend between the raw sky-view in-scatter
    // (bluer, crisp aerial perspective) and the warmed land veil. warmth=1
    // reproduces the prior FarLodHorizon-safe warm haze; warmth<1 lets distance
    // read with cooler atmospheric blue when the look calls for it.
    inscatter = mix(rawInscatter, inscatter, clamp(u_atmosphereWarmth, 0.0, 1.0));

    //  horizon blowout fix: this pass composites OVER the lighting FBO, which
    // is already tonemapped + gamma display-space (sRGB 0..1), but `inscatter` is
    // HDR-linear radiance (sky-view * strength). Compositing it raw drove distant
    // terrain past pure white ("overpaying in the distance"). Map the in-scatter
    // through the SAME filmic tonemap + gamma so it lands in display range and
    // matches the sky dome's horizon brightness — distant terrain now fades INTO
    // the sky instead of over-brightening past it.
    inscatter = inscatter * (2.51 * inscatter + 0.03) /
                (inscatter * (2.43 * inscatter + 0.59) + 0.14);
    inscatter = pow(max(inscatter, vec3(0.0)), vec3(1.0 / 2.2));

    // VALLEY MIST is luminous (pale), not just the warm sky hue over dark ground.
    // Lift the in-scatter toward a pale warm-white where the ground fog dominates so
    // morning/evening mist reads as actual mist filling the valleys. Tinted slightly
    // by the warm low-sun hue so it stays palette-coherent.
    float mistAmt = clamp(groundFog / 0.9, 0.0, 1.0);
    vec3 mistColor = mix(vec3(0.82, 0.84, 0.90), aHue, 0.35); // pale, faintly warm
    inscatter = mix(inscatter, mistColor, mistAmt * 0.7);

    // Keep a hint of far terrain rather than a pure sky veil so the long view
    // still reads (Distant-Horizons style) instead of dissolving to flat sky.
    fog = min(fog, 0.94);

    //  froxel volumetrics (, ): compose the froxel media IN FRONT
    // of the analytic haze — total = froxelL + T_froxel * analytic. The volume's
    // W coordinate is the exponential-slice mapping of the RADIAL view distance
    // (FroxelGrid.h DepthToTextureW).
    if (u_volumetricMode == 1) {
        float dClamped = clamp(dist, FROXEL_NEAR, FROXEL_FAR);
        float w = clamp(log(dClamped / FROXEL_NEAR) / log(FROXEL_FAR / FROXEL_NEAR), 0.0, 1.0);
        vec4 fx = texture(u_froxelIntegrated, vec3(TexCoords, w));
        // Same display mapping the analytic in-scatter goes through (this pass
        // composites over an already-tonemapped target).
        vec3 fxL = fx.rgb;
        fxL = fxL * (2.51 * fxL + 0.03) / (fxL * (2.43 * fxL + 0.59) + 0.14);
        fxL = pow(max(fxL, vec3(0.0)), vec3(1.0 / 2.2));
        float fxFog = clamp(1.0 - fx.a, 0.0, 1.0);
        vec3 blended_num = inscatter * fog * fx.a + fxL * fxFog;
        float outFog = clamp(fog * fx.a + fxFog, 0.0, 0.94);
        if (outFog > 1e-4) {
            inscatter = blended_num / outFog;
        }
        fog = outFog;
    }

    // alpha = fog composites the aerial haze OVER the lit terrain.
    FragColor = vec4(inscatter, fog);
}
