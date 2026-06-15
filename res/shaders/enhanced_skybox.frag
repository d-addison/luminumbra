#version 450 core
out vec4 FragColor;

in vec3 WorldPos;

// Enhanced atmospheric uniforms
uniform vec3 u_sunDirection;
uniform vec3 u_moonDirection;
uniform float u_sunIntensity;
// T-I4-DR-tod-sky-balance: continuous day->twilight->night factor from the sun
// elevation (1 sun high, ~0 sun below horizon). Drives the dome's brightness
// and tint so the dusk dome warms/darkens and the night dome goes genuinely
// dark, instead of riding the clamped u_sunIntensity that saturates to 1 while
// the sun is still low. Defaulted so older callers fall back to a lit day dome.
uniform float u_skyDayFactor = 1.0;
uniform float u_time;
uniform float u_atmosDensity = 1.0;
uniform float u_cloudCoverage = 0.5;
uniform vec3 u_skyTint = vec3(1.0, 0.95, 0.8);

// T-I5a-8 (C3): wind-advected 2.5D cloud coverage field. The coverage is a pure
// function of world XZ position, a wind-driven scroll offset, the weather-derived
// coverage amount, and a biome variation factor. The IDENTICAL coverage function
// (cloudCoverageAt below) is evaluated here for the sky-dome cloud layer AND in
// lighting_pass.frag for the projected cast shadow, so the drifting dome clouds
// and the crawling terrain shadows stay registered. Render-only: nothing here
// writes back into any sim/world_hash input (critique F2, one-way render->sim).
//   u_cloudScrollOffset  - wind * tick-phase, in world metres (drift vector)
//   u_cloudCoverageAmount - [0,1] sky fraction the weather state wants covered
//   u_cloudBiomeVariation - biome-driven coverage bias (e.g. wetter biomes cloudier)
//   u_cloudPlaneHeight    - world Y of the cloud sheet (for dome projection + shadow)
//   u_cloudShadowStrength - how much the projected coverage darkens the sun
uniform vec2  u_cloudScrollOffset = vec2(0.0);
uniform float u_cloudCoverageAmount = 0.45;
uniform float u_cloudBiomeVariation = 0.0;
uniform float u_cloudPlaneHeight = 900.0;
uniform float u_cloudShadowStrength = 0.0;

// T-I5a-6: PBR atmospheric scattering. The authored vertical gradient + ad-hoc
// rayleigh/mie haze is replaced by the precomputed Hillaire 2020 sky-view LUT
// (192x108 sky dome radiance for the current sun) plus the transmittance LUT
// (256x64). The sun disc is colored by the transmittance toward the sun, so the
// disc, the dome, the lighting-pass ambient and the aerial-perspective fog all
// share ONE transmittance and the low-sun palette (pinks/purples/oranges from
// Rayleigh/Mie at long optical paths) emerges coherently. u_skyDayFactor is
// kept as the night-darkening brightness envelope; the star + aurora layers are
// kept unchanged. u_skyExposure scales LUT radiance into the display range.
uniform sampler2D u_skyViewLut;
uniform sampler2D u_transmittanceLut;
uniform float u_sunCosZenith = 1.0;   // dot(toward-sun, up)
uniform float u_skyExposure = 38.0;   // LUT radiance -> HDR display scale
uniform int u_useSkyLut = 1;          // 0 falls back to the legacy gradient

const float PI_SKY = 3.14159265359;

// Enhanced noise functions for atmospheric effects
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f*f*(3.0-2.0*f);
    float a = hash(i + vec2(0.,0.));
    float b = hash(i + vec2(1.,0.));
    float c = hash(i + vec2(0.,1.));
    float d = hash(i + vec2(1.,1.));
    return mix(mix(a,b,f.x), mix(c,d,f.x), f.y);
}

// Fractal noise for clouds
float fbm(vec2 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for(int i = 0; i < octaves; i++) {
        value += amplitude * noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}

// T-I5a-8 (C3): the SHARED wind-advected cloud coverage field. Returns the cloud
// optical density [0,1] at a world XZ position. This EXACT function is duplicated
// verbatim in lighting_pass.frag (GLSL has no shared includes here); the dome
// clouds below and the projected cast shadow there evaluate the same field at the
// same world XZ, so a dome cloud and its ground shadow stay registered as both
// drift with the wind. f(noise, wind offset, weather coverage, biome):
//   - the world XZ is scrolled by u_cloudScrollOffset (wind * tick-phase) so the
//     whole field translates with the large-scale wind direction;
//   - two fbm octave-stacks at different scales give billowy structure;
//   - u_cloudCoverageAmount (+ biome bias) sets the smoothstep threshold so a
//     low coverage yields sparse fair-weather puffs and a high coverage an
//     overcast sheet. PARTLY-CLOUDY is the mid range the CloudShadow gate uses.
float cloudCoverageAt(vec2 worldXZ) {
    // Metres -> noise units. ~1200 m feature scale for the main cloud cells.
    vec2 p = (worldXZ + u_cloudScrollOffset) * (1.0 / 1200.0);
    float base = fbm(p, 5);
    float detail = fbm(p * 2.7 + vec2(11.3, 4.7), 3);
    float field = base * 0.72 + detail * 0.28;
    // Coverage threshold: higher coverage -> lower threshold -> more sky covered.
    float cov = clamp(u_cloudCoverageAmount + u_cloudBiomeVariation, 0.0, 1.0);
    float lo = mix(0.62, 0.30, cov);
    float hi = mix(0.82, 0.55, cov);
    return smoothstep(lo, hi, field);
}

// T-I5a-6: sample the sky-view LUT for a view direction. u = azimuth around the
// sun [0,2pi]->[0,1]; v = view zenith [0 (up), pi (down)]->[0,1].
vec3 sampleSkyView(vec3 viewDir) {
    float cosV = clamp(viewDir.y, -1.0, 1.0);
    float zenith = acos(cosV);
    vec2 vh = normalize(vec2(viewDir.x, viewDir.z) + 1e-5);
    vec2 sh = normalize(vec2(u_sunDirection.x, u_sunDirection.z) + 1e-5);
    float az = acos(clamp(dot(vh, sh), -1.0, 1.0));   // [0, pi]
    float u = az / (2.0 * PI_SKY);
    float v = clamp(zenith / PI_SKY, 0.0, 1.0);
    return texture(u_skyViewLut, vec2(u, v)).rgb;
}

// Transmittance toward the sun for a ground viewer (mu = cos sun-zenith remapped
// [-1,1]->[0,1]; altitude row 0).
vec3 sunTransmittance(float cosZenith) {
    float u = clamp((cosZenith + 1.0) * 0.5, 0.0, 1.0);
    return texture(u_transmittanceLut, vec2(u, 0.0)).rgb;
}

// T-I5a-8 (C3): wind-advected sky-dome cloud layer + landscape-distance imposters.
// The view ray is intersected with the cloud plane (u_cloudPlaneHeight); the hit's
// world XZ feeds the SHARED cloudCoverageAt field, so the dome clouds are the same
// field that casts the ground shadow and they DRIFT with the wind as the scroll
// offset advances. Rays toward the horizon hit the plane far away -> the cloud
// cells foreshorten into fluffy landscape-distance imposters near the horizon
// band; rays toward the zenith sample the overhead sheet. Render-only.
// T-I4-DR-tod-sky-balance: dayFactor lights the clouds; at night they fall to a
// faint dark silhouette instead of holding a lit sunset tint over the dome.
vec3 renderClouds(vec3 viewDir, vec3 baseColor, float dayFactor) {
    if (viewDir.y < 0.02) return baseColor; // No clouds at/below the horizon

    // Eye assumed near the origin of the (translation-stripped) sky cube. Project
    // the ray up to the cloud plane: t = planeHeight / viewDir.y. The resulting
    // XZ is in metres, so it lines up with the lighting pass world XZ.
    float t = u_cloudPlaneHeight / max(viewDir.y, 1e-3);
    vec2 worldXZ = viewDir.xz * t;

    // Distance fade so the far (near-horizon) cloud band thins into haze rather
    // than tiling hard -- this is the "imposter" foreshortening band.
    float horizonFade = smoothstep(0.02, 0.22, viewDir.y);

    float coverage = cloudCoverageAt(worldXZ);
    // A faint higher detail octave breaks up the silhouette near the zenith.
    float detail = fbm(worldXZ * (1.0 / 520.0) + u_cloudScrollOffset * (1.0 / 520.0), 3);
    coverage = clamp(coverage * (0.82 + 0.18 * detail), 0.0, 1.0);
    coverage *= horizonFade;

    // Self-shadow: denser cloud cores read darker on their sun-away side.
    vec3 litCloud = vec3(0.95, 0.96, 1.0);
    vec3 shadowCloud = vec3(0.55, 0.57, 0.66);
    float sunDot = dot(viewDir, u_sunDirection);
    float cloudLighting = max(0.35, sunDot * 0.5 + 0.5);
    vec3 cloudColor = mix(shadowCloud, litCloud, cloudLighting);
    // Night tint + darkening (kept from the old layer so night clouds silhouette).
    cloudColor = mix(cloudColor, vec3(0.18, 0.16, 0.26), 1.0 - dayFactor);
    cloudColor *= dayFactor + 0.04 * dayFactor;

    return mix(baseColor, cloudColor, coverage);
}

// Enhanced star field
vec3 renderStars(vec3 viewDir, float nightIntensity) {
    if(nightIntensity < 0.1) return vec3(0.0);

    float stars1 = noise(viewDir.xy * 800.0);
    float stars2 = noise(viewDir.xz * 1200.0 + vec2(100.0, 200.0));
    float stars3 = noise(viewDir.yz * 600.0 + vec2(300.0, 400.0));

    float brightStars = smoothstep(0.99, 1.0, stars1);
    float mediumStars = smoothstep(0.985, 0.995, stars2) * 0.7;
    float dimStars = smoothstep(0.97, 0.98, stars3) * 0.4;

    float twinkle = 0.8 + 0.4 * sin(u_time * 3.0 + viewDir.x * 1000.0);

    vec3 starColor = vec3(0.9, 0.9, 1.0);
    float totalStars = (brightStars + mediumStars + dimStars) * twinkle * nightIntensity;

    return starColor * totalStars;
}

// Aurora effect for magical atmosphere
vec3 renderAurora(vec3 viewDir, float nightIntensity) {
    if(nightIntensity < 0.3 || viewDir.y < 0.2) return vec3(0.0);

    float auroraTime = u_time * 0.1;
    vec2 auroraUV = vec2(viewDir.x, viewDir.y) * 3.0;

    float aurora1 = sin(auroraUV.x * 2.0 + auroraTime) * cos(auroraUV.y + auroraTime * 0.7);
    float aurora2 = sin(auroraUV.x * 1.5 - auroraTime * 0.8) * cos(auroraUV.y * 1.3 - auroraTime);

    float auroraFlow = fbm(auroraUV + vec2(auroraTime, -auroraTime * 0.5), 3);

    float auroraIntensity = (aurora1 + aurora2) * auroraFlow;
    auroraIntensity = smoothstep(0.2, 0.8, abs(auroraIntensity)) * nightIntensity * 0.3;

    vec3 auroraColor = mix(
        vec3(0.2, 0.8, 0.4),
        vec3(0.6, 0.2, 0.9),
        sin(auroraTime + viewDir.x * 5.0) * 0.5 + 0.5
    );

    return auroraColor * auroraIntensity;
}

// Legacy authored gradient (kept as the u_useSkyLut == 0 fallback so the dome is
// never blank if the LUT is unavailable).
vec3 legacyGradient(vec3 viewDir, float dayFactor) {
    vec3 dayTopColor = vec3(0.22, 0.45, 0.92) * u_skyTint;
    vec3 dayBottomColor = vec3(0.9, 0.95, 1.0) * u_skyTint;
    vec3 nightTop = vec3(0.002, 0.004, 0.012);
    vec3 nightBottom = vec3(0.006, 0.012, 0.03);
    vec3 topColor = mix(nightTop, dayTopColor, dayFactor);
    vec3 bottomColor = mix(nightBottom, dayBottomColor, dayFactor);
    float horizonBlend = smoothstep(-0.2, 0.6, viewDir.y);
    return mix(bottomColor, topColor, horizonBlend);
}

void main()
{
    vec3 viewDir = normalize(WorldPos);
    float dayFactor = clamp(u_skyDayFactor, 0.0, 1.0);
    float nightFactor = 1.0 - dayFactor;

    // T-I5a-6: aerial warm-grade factors, computed once and applied POST-tonemap
    // (section 7). The dome is bright (~0.8-1.0 after exposure), which is exactly
    // the ACES saturating region where R,G,B all crush toward white -- a
    // pre-tonemap chroma tint is flattened back to neutral (the root-cause "the
    // tonemap eats the warmth" failure that left the dome too-blue at noon and
    // paradoxically BLUER at dusk). So the warm shift is applied to the FINAL
    // tonemapped color where it survives. The hue is the physical sun-path
    // transmittance (blue scattered OUT along the long aerial path), shared with
    // the sun disc / aerial fog -> one coherent warm palette, no authored color.
    vec3 aerialTrans = sunTransmittance(u_sunCosZenith);
    float aerialNorm = max(aerialTrans.r, max(aerialTrans.g, aerialTrans.b));
    vec3 aerialHue = aerialTrans / max(aerialNorm, 1e-4);   // pure hue, peak = 1
    // Deepen the hue so the bright midday sky still resolves a clearly warm
    // (pale-amber) band rather than the too-blue Rayleigh dome. R stays ~1
    // (peak), G/B pushed DOWN (exponent > 1 on a sub-unit value shrinks it). At
    // low sun the hue is already deeply red so this saturates harmlessly.
    aerialHue = pow(clamp(aerialHue, vec3(0.0), vec3(1.0)), vec3(1.0, 1.45, 2.6));
    float aerialHorizonF = 1.0 - 0.55 * smoothstep(0.25, 1.0, max(viewDir.y, 0.0));
    float aerialTowardSun = 0.80 + 0.20 * clamp(dot(viewDir, u_sunDirection), -1.0, 1.0);
    // Sun-elevation ramp: enough at noon to lift the SkyboxVisual horizon band
    // over the warm threshold, rising to a STRONGER warm at low sun so dusk warms
    // MORE than noon (the required dusk-over-noon warm SHIFT). The luminance-
    // preserving post grade means raising noon warmth does NOT brighten the dome.
    float aerialLowSun = mix(1.05, 1.45, 1.0 - smoothstep(0.10, 0.85, u_sunCosZenith));
    // Night fade: hold the warm grade through dusk (dayFactor ~0.43); only fade
    // it out at deep night. A plain `* dayFactor` halved the warmth at dusk.
    float aerialNightFade = smoothstep(0.0, 0.22, dayFactor);
    float aerialWarm = clamp(aerialHorizonF * aerialTowardSun * aerialLowSun, 0.0, 1.0)
                       * aerialNightFade;

    // --- 1. SCATTERING COLOR FROM THE SKY-VIEW LUT ---
    // The LUT supplies the COLOR (coherent low-sun pinks/oranges); u_skyDayFactor
    // supplies the night-darkening brightness envelope so the dome still goes
    // genuinely dark at night and stars/moon read against it.
    vec3 skyColor;
    if (u_useSkyLut != 0) {
        vec3 lutRadiance = sampleSkyView(viewDir) * u_skyExposure;
        // T-I5a-6 FIX: the night envelope must DARKEN the warm scattering, not
        // CROSS-FADE it to a fixed blue base. The old `mix(nightBase, lut,
        // dayFactor)` blended ~57% deep-blue base into the dusk dome (dayFactor
        // ~0.43 at the t=0.22 dusk), pulling the sun-side r/b DOWN below noon --
        // the "dusk got bluer" regression. Instead we (1) scale the LUT radiance
        // by dayFactor so the dome darkens through dusk into night while KEEPING
        // its warm scattering hue, and (2) add a tiny deep-night ADDITIVE floor
        // that only matters once dayFactor ~ 0 (true night), so stars/moon still
        // read against a dark dome. The warm low-sun palette now survives dusk.
        // T-I5a-6 FIX (aerial reddening of the dome): the sky-view LUT in-scatter
        // is Rayleigh/multi-scatter blue-dominant at ALL sun angles, and at a low
        // sun the (faint, reddened) single-scatter is overpowered by the
        // ISOTROPIC multi-scatter blue floor, so the dome paradoxically read
        // BLUER at dusk than noon. The terrain already warms (m_sun.color carries
        // the sun-path transmittance); the DOME must redden the same way. The
        // in-scattered sunlight reaching the eye along a near-horizon / toward-sun
        // ray traversed a long atmospheric path, so it is the sun-path
        // transmittance (blue scattered OUT) that survives. We tint the dome
        // toward the chromatic sun transmittance, normalized to a pure HUE shift
        // (so luminance/exposure are preserved and noon-overhead -- neutral
        // transmittance -- is untouched), weighted by a horizon factor, a
        // toward-sun factor, and a LOW-SUN factor so the effect vanishes at high
        // noon and rises as the sun drops. This is the same transmittance the sun
        // disc + aerial fog use -> one coherent warm palette, no authored color.
        // The aerial warm grade is applied POST-tonemap (section 7) only, so it
        // does NOT darken the HDR scattering here (a pre-tonemap multiply by the
        // sub-unit warm hue dimmed the dome and broke the noon>dusk luminance
        // ordering). The LUT radiance feeds the tonemap at full brightness.
        vec3 nightFloor = mix(vec3(0.006, 0.012, 0.03), vec3(0.002, 0.004, 0.012),
                              smoothstep(-0.2, 0.6, viewDir.y));
        skyColor = (lutRadiance * dayFactor + nightFloor * nightFactor) * u_atmosDensity;
    } else {
        skyColor = legacyGradient(viewDir, dayFactor) * u_atmosDensity;
    }

    // --- 2. SUN DISC (colored by the transmittance toward the sun) ---
    float sunDot = dot(viewDir, u_sunDirection);
    // Sun disc / corona ride dayFactor so a low dusk sun renders a soft warm disc
    // and the disc fades out below the horizon.
    float sunMask = smoothstep(0.9985, 0.9999, sunDot) * dayFactor;
    float sunCorona = pow(max(0.0, sunDot), 32.0) * dayFactor * 0.5;
    sunCorona *= smoothstep(0.0, 0.2, viewDir.y); // fade near horizon

    // T-I5a-6: the disc color IS the atmospheric transmittance toward the sun
    // (same LUT the lighting pass + aerial fog read). At low sun the long path
    // eats blue first, so the disc reddens to deep orange coherently with the
    // warm horizon scattering -- no hand-authored sunset color.
    vec3 sunTrans = sunTransmittance(u_sunCosZenith);
    vec3 sunColor = sunTrans * 4.0;
    skyColor += sunColor * (sunMask + sunCorona);

    // --- 3. MOON ---
    float moonDot = dot(viewDir, u_moonDirection);
    float moonMask = smoothstep(0.996, 0.9999, moonDot) * nightFactor;
    float moonGlow = pow(max(0.0, moonDot), 16.0) * nightFactor * 0.2;
    float craterNoise = fbm(viewDir.xy * 25.0, 4);
    vec3 moonSurface = vec3(0.8, 0.8, 0.7) * (0.7 + 0.3 * craterNoise);
    vec3 moonGlowColor = vec3(0.8, 0.9, 1.0) * moonGlow;
    skyColor = mix(skyColor + moonGlowColor, moonSurface, moonMask);

    // --- 4. CLOUDS WITH ATMOSPHERIC LIGHTING ---
    skyColor = renderClouds(viewDir, skyColor, dayFactor);

    // --- 5. STARS ---
    skyColor += renderStars(viewDir, nightFactor);

    // --- 6. MAGICAL AURORA ---
    skyColor += renderAurora(viewDir, nightFactor);

    // --- 7. HDR TONEMAPPING ---
    skyColor = skyColor * (2.51 * skyColor + 0.03) / (skyColor * (2.43 * skyColor + 0.59) + 0.14);

    // T-I5a-6: POST-tonemap aerial warm grade. The bright dome lives in the ACES
    // saturating region where a pre-tonemap chroma tint is crushed back to white,
    // so the warm sun-path-transmittance hue is re-applied HERE where it survives.
    // Implemented as a luminance-preserving channel rescale: push the tonemapped
    // color toward the warm hue without darkening (divide-by-mean keeps overall
    // brightness, so the luminance-ordering / PlayerView gates are unaffected).
    if (u_useSkyLut != 0) {
        const vec3 kLumaW = vec3(0.2126, 0.7152, 0.0722); // gate luminance weights
        vec3 tint = mix(vec3(1.0), aerialHue, clamp(aerialWarm, 0.0, 1.0));
        // Normalize by the LUMINANCE of the tint (not its arithmetic mean) so the
        // grade is a pure hue rotation that leaves perceived luminance EXACTLY
        // unchanged -- the warm shift cannot brighten the dusk dome relative to
        // noon (which previously inverted the noon>dusk luminance ordering).
        float tintLuma = max(dot(tint, kLumaW), 1e-4);
        skyColor *= tint / tintLuma;
    }

    // Color grading for fantasy atmosphere
    skyColor = pow(skyColor, vec3(0.9, 0.95, 1.05));

    // --- 8. GAMMA CORRECTION ---
    skyColor = pow(skyColor, vec3(1.0/2.2));

    FragColor = vec4(skyColor, 1.0);
}
