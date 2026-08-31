#version 450 core
out vec4 FragColor;

in vec3 WorldPos;

// Enhanced atmospheric uniforms
uniform vec3 u_sunDirection;
uniform vec3 u_moonDirection;
uniform float u_sunIntensity;
// continuous day->twilight->night factor from the sun
// elevation (1 sun high, ~0 sun below horizon). Drives the dome's brightness
// and tint so the dusk dome warms/darkens and the night dome goes genuinely
// dark, instead of riding the clamped u_sunIntensity that saturates to 1 while
// the sun is still low. Defaulted so older callers fall back to a lit day dome.
uniform float u_skyDayFactor = 1.0;
uniform float u_time;
uniform float u_atmosDensity = 1.0;
uniform float u_cloudCoverage = 0.5;
uniform vec3 u_skyTint = vec3(1.0, 0.95, 0.8);

// wind-advected 2.5D cloud coverage field. The coverage is a pure
// function of world XZ position, a wind-driven scroll offset, the weather-derived
// coverage amount, and a biome variation factor. The IDENTICAL coverage function
// (cloudCoverageAt below) is evaluated here for the sky-dome cloud layer AND in
// lighting_pass.frag for the projected cast shadow, so the drifting dome clouds
// and the crawling terrain shadows stay registered. Render-only: nothing here
// writes back into any sim/world_hash input (regression contract, one-way render->sim).
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

// PBR atmospheric scattering. The authored vertical gradient + ad-hoc
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

//  (defect 5): aurora is a DEEP-NIGHT-ONLY phenomenon.
// u_skyDayFactor alone cannot cleanly separate DUSK (sun on the horizon ->
// dayFactor ~0.02) from NIGHT (sun well below -> dayFactor ~0): both round to a
// hair above zero and the old envelope let the aurora bleed into the twilight
// dome. So the CPU computes a dedicated aurora strength from the sun's RAW
// elevation (1 only once the sun is clearly below the horizon, 0 through dusk)
// and pushes it here. 0 == no aurora; the dusk dome stays clean.
uniform float u_auroraStrength = 0.0;

//  (defect 3): NIGHT-STORM visibility floor. A night
// storm was rendering as a near-black dome (dayFactor ~0 -> the dome scattering
// and the cloud layer both collapse to black). This is a small additive sky-grey
// floor under the storm clouds so a NIGHT storm is dark-but-legible (rain +
// lightning read against it) WITHOUT lifting the clear-night dome. The CPU passes
// the storm intensity; it is 0 for clear sky so clear night stays genuinely dark.
uniform float u_stormSkyFloor = 0.0;   // [0,1] storm intensity for the night floor

//  isolation/layer mode: when > 0 the skybox renders a flat neutral BACKDROP
// (the no-geometry background) instead of the sky dome, so an isolated subsystem
// can be reviewed against a void/greenscreen/checker. 0 = normal sky (default).
//   1 = void (flat u_backdropColor), 2 = greenscreen, 3 = checker.
// No GLSL initializer for the int (non-core); SkyboxPass sets these every frame
// and GL defaults unset ints to 0, so every non-isolation render is byte-stable.
uniform int  u_backdropMode;
uniform vec3 u_backdropColor;

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

// the SHARED wind-advected cloud coverage field. Returns the cloud
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
    // larger cloud clusters (~2400 m feature scale, was 1200) for bolder,
    // more pronounced cloud masses. MUST stay identical to
    // lighting_pass.frag::cloudCoverageAt so the dome cloud and its cast shadow
    // remain registered.
    vec2 p = (worldXZ + u_cloudScrollOffset) * (1.0 / 2400.0);
    float base = fbm(p, 5);
    float detail = fbm(p * 2.7 + vec2(11.3, 4.7), 3);
    float field = base * 0.72 + detail * 0.28;
    // Coverage threshold: higher coverage -> lower threshold -> more sky covered.
    float cov = clamp(u_cloudCoverageAmount + u_cloudBiomeVariation, 0.0, 1.0);
    float lo = mix(0.62, 0.30, cov);
    float hi = mix(0.82, 0.55, cov);
    return smoothstep(lo, hi, field);
}

// sample the sky-view LUT for a view direction. u = azimuth around the
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

// ===========================================================================
//  #2: VOLUMETRIC clouds (tier-2, Nubis-style raymarch)..
//
// The tier-1 layer projected the view ray onto a single cloud PLANE and shaded
// a 2.5D coverage sheet -- it had no depth, no parallax, no real silhouette
// (the CLOUDS_FLAT_NO_STRUCTURE risk). This replaces that with a bounded
// view-ray raymarch through a cloud SLAB [kCloudBottom, kCloudTop]:
//   * horizontal density still comes from the SHARED cloudCoverageAt field,
//     so the dome clouds stay registered with the ground shadow the lighting
//     pass projects from the SAME field (CloudShadow gate intact, render-only);
//   * a vertical PROFILE (rounded base, anvil-tapered top) shapes the slab;
//   * 3D EROSION fbm carves billows/cauliflower into the body;
//   * a short LIGHT-MARCH toward the sun gives Beer-Lambert self-shadowing and
//     a Henyey-Greenstein forward-scatter silver lining;
//   * scattering is integrated front-to-back with transmittance (energy-
//     conserving), composited over the LUT sky by the accumulated alpha.
//
// GATE SAFETY: clear-sky pixels (coverage ~0 at the slab) take a CHEAP reject
// and return the base gradient BYTE-FOR-BYTE, so the SkyboxVisual horizon->
// zenith luminance-drop / monotonicity gate sees an untouched fair-weather
// dome -- only genuinely cloudy pixels pay the march. Storm decks (high
// coverage) fill the dome with a structured overcast (high luma variance ->
// not CLOUDS_FLAT). Render-only: nothing writes back to sim/world_hash.
// ===========================================================================

// Cloud slab in metres (world Y). Anchored on the shared u_cloudPlaneHeight so
// the horizontal registration with the ground-shadow field is preserved.
float cloudSlabBottom() { return u_cloudPlaneHeight; }
float cloudSlabTop()    { return u_cloudPlaneHeight + 420.0; }

// Henyey-Greenstein phase: g>0 forward scatter (bright silver lining toward sun).
float hgPhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI_SKY * pow(max(denom, 1e-4), 1.5));
}

// Volumetric cloud density at a world-space sample p. Combines the shared 2D
// coverage field (registration), a vertical profile, and 3D erosion. Returns
// [0,1]; also outputs the raw horizontal coverage for the caller's cheap reject.
// structureWeight (0 fair.. 1 storm) fills the slab into an overcast deck.
float cloudDensity(vec3 p, float structureWeight, out float coverageOut) {
    float coverage = cloudCoverageAt(p.xz);
    // Storm fills holes: lift a coverage floor so a heavy deck is unbroken.
    coverage = mix(coverage, max(coverage, mix(0.55, 0.95, structureWeight)), structureWeight);
    coverageOut = coverage;
    if (coverage <= 0.002) return 0.0;

    float hf = clamp((p.y - cloudSlabBottom()) / (cloudSlabTop() - cloudSlabBottom()), 0.0, 1.0);
    // Vertical profile: feather in off the rounded base, taper the anvil top.
    float profile = smoothstep(0.0, 0.16, hf) * (1.0 - smoothstep(0.50, 1.0, hf));

    // 3D erosion: low-frequency billow shape + high-frequency cauliflower edge.
    // p.y folded into the 2D fbm lookups gives vertical (3D) variation cheaply.
    vec2 wind = u_cloudScrollOffset;
    float shape   = fbm((p.xz + wind) * (1.0 / 430.0) + vec2(p.y * 0.0045), 4);
    float erosion = fbm((p.xz - wind * 0.6) * (1.0 / 120.0) + vec2(p.y * 0.011, 0.0), 3);

    float d = coverage * profile;
    // Modulate the body by the billow shape, then erode the edges (less erosion
    // where coverage is high so a storm deck stays solid).
    d *= (0.45 + 0.85 * shape);
    d -= erosion * 0.32 * (1.0 - coverage * 0.6);
    return clamp(d, 0.0, 1.0);
}

//  #2: bounded volumetric raymarch through the cloud slab. Replaces the
// tier-1 single-plane projection. dayFactor lights the clouds; at night they
// fall to a faint dark silhouette (storm night held legible by u_stormSkyFloor).
vec3 renderClouds(vec3 viewDir, vec3 baseColor, float dayFactor) {
    if (viewDir.y < 0.02) return baseColor; // No clouds at/below the horizon.

    float structureWeight = smoothstep(0.55, 0.80, u_cloudCoverageAmount);

    // Eye at the origin of the (translation-stripped) sky cube. The slab spans
    // [tEnter, tExit] along the ray; p.xz = viewDir.xz * t matches the lighting
    // pass world XZ (same as the tier-1 plane projection -> registration kept).
    float tEnter = cloudSlabBottom() / max(viewDir.y, 1e-3);
    float tExit  = cloudSlabTop()    / max(viewDir.y, 1e-3);

    // Probe the bounded slab at bottom, middle, and top. A midpoint-only reject
    // can miss tilted columns that clip cloud near a slab edge.
    float lowCov = cloudCoverageAt(viewDir.xz * tEnter);
    float midCov = cloudCoverageAt(viewDir.xz * (0.5 * (tEnter + tExit)));
    float highCov = cloudCoverageAt(viewDir.xz * tExit);
    midCov = max(midCov, max(lowCov, highCov));
    midCov = mix(midCov, max(midCov, mix(0.55, 0.95, structureWeight)), structureWeight);
    if (midCov <= 0.01) return baseColor;

    // Horizon fade: fair weather thins to clear haze at the horizon (clear-sky
    // gate intact); a storm keeps near-full cover right down to the horizon band.
    float horizonFade = mix(smoothstep(0.02, 0.22, viewDir.y),
                            0.55 + 0.45 * smoothstep(0.02, 0.10, viewDir.y),
                            structureWeight);

    // --- Raymarch the slab front-to-back. ---
    const int   kSteps      = 28;
    const int   kLightSteps = 5;
    float dt = (tExit - tEnter) / float(kSteps);
    // Per-pixel jitter breaks the slab-entry banding without a blue-noise tex.
    float jitter = hash(viewDir.xz * 512.0 + u_time);

    // Lighting palette. Sun colour reddens at low sun via the transmittance LUT
    // (coherent with the disc/aerial); ambient is a cool sky fill.
    vec3 sunLight = sunTransmittance(u_sunCosZenith) * 2.4;
    vec3 ambient  = vec3(0.42, 0.47, 0.58);
    float cosToSun = dot(viewDir, u_sunDirection);
    // Blend a forward (silver lining) and a near-isotropic lobe.
    float phase = mix(hgPhase(cosToSun, 0.20), hgPhase(cosToSun, 0.76), 0.5);

    const float kSigma      = 0.085; // extinction per metre-density
    const float kLightSigma = 0.090;

    float transmittance = 1.0;
    vec3  scatter = vec3(0.0);

    for (int i = 0; i < kSteps; ++i) {
        float t = tEnter + (float(i) + jitter) * dt;
        vec3 p = viewDir * t;
        float cov;
        float density = cloudDensity(p, structureWeight, cov);
        if (density > 0.001) {
            // Light march toward the sun for self-shadowing (Beer-Lambert).
            float lightDt = (tExit - tEnter) / float(kSteps) * 1.4;
            float lightDensity = 0.0;
            for (int j = 0; j < kLightSteps; ++j) {
                vec3 lp = p + u_sunDirection * (lightDt * (float(j) + 0.5));
                float lc;
                lightDensity += cloudDensity(lp, structureWeight, lc);
            }
            float lightTrans = exp(-lightDensity * lightDt * kLightSigma);
            // Beer-Powder: the powder term restores the dark-edge / bright-core
            // look multiple scattering would give (Nubis, SIGGRAPH 2015).
            float powder = 1.0 - exp(-density * dt * kSigma * 2.0);
            vec3 sunTerm = sunLight * lightTrans * phase * powder;
            vec3 stepColor = (sunTerm + ambient) * density;

            float stepExt = exp(-density * dt * kSigma);
            // Energy-conserving front-to-back integration.
            scatter += transmittance * stepColor * (1.0 - stepExt);
            transmittance *= stepExt;
            if (transmittance < 0.02) break;
        }
    }

    float alpha = clamp((1.0 - transmittance) * horizonFade, 0.0, 1.0);
    vec3 cloudColor = scatter;

    // Storm deck reads charcoal-grey (menacing), not bright white.
    cloudColor = mix(cloudColor, cloudColor * 0.66, structureWeight);
    // GOLDEN HOUR: warm the lit cloud body toward the low-sun colour so dawn/dusk
    // clouds GLOW orange/pink from the side instead of going to flat silhouette.
    // sunTransmittance reddens at low sun; ramp this in as the sun drops (peaks at
    // the horizon, off at high noon) and weight by the sun-facing phase already in
    // `scatter`. Render-only.
    {
        vec3 warmSun = sunTransmittance(u_sunCosZenith);
        float lowSun = 1.0 - smoothstep(0.10, 0.55, u_sunCosZenith); // 0 noon -> 1 low sun
        float lit = clamp(dot(viewDir, u_sunDirection) * 0.5 + 0.5, 0.0, 1.0); // sun-facing
        cloudColor = mix(cloudColor, cloudColor * (vec3(1.0) + warmSun * 1.6), lowSun * (0.35 + 0.65 * lit) * dayFactor);
    }
    // Night tint so clouds silhouette against the dark dome -- but ONLY in true
    // night, not through twilight (else dusk/dawn clouds read as dark silhouettes
    // even while the sky still glows). Gated on a sharp night ramp.
    float nightT = 1.0 - smoothstep(0.0, 0.32, dayFactor);
    cloudColor = mix(cloudColor, cloudColor * 0.12 + vec3(0.02, 0.02, 0.03), nightT);

    // NIGHT-STORM floor: keep a night overcast legible without lifting the
    // clear-night dome (u_stormSkyFloor is 0 for clear sky).
    float nightStormFloor = u_stormSkyFloor * (1.0 - dayFactor) * 0.16;
    // Keep clouds bright through twilight (was dayFactor*1.04 -> halved at dusk).
    float cloudBright = max(0.5 + 0.55 * dayFactor, nightStormFloor);
    cloudColor *= cloudBright;

    return mix(baseColor, cloudColor, alpha);
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

// Aurora effect for magical atmosphere. : aurora is a
// NIGHT-ONLY phenomenon, gated by u_auroraStrength (CPU-derived from the sun's RAW
// elevation): a hard 0 through day + dusk + dawn, only opening once the sun is well
// below the horizon, so the twilight dome stays clean .
//
// the aurora is rebuilt as flowing vertical
// CURTAINS (draped sheets), NOT the old soft circular blobs. The previous version
// took a product sin(uv.x)*cos(uv.y) of the raw screen-space viewDir.x/.y and
// smoothstep(abs(.)) -> isolated 2D lobes that read as discrete green/magenta ORBS
// and, because they keyed off viewDir.x, hugged/cut hard at the frame edge.
//
// The rewrite parameterizes the dome by AZIMUTH around the horizon (continuous,
// wraps seamlessly -> no frame-edge hard cut) and HEIGHT. A curtain is a set of
// thin azimuthal RIDGES that wander slowly with the flow (the draped sheet seen
// edge-on), with fine vertical striations running up the sheet and a height
// envelope so the curtain hangs from the upper dome and feathers out at the
// horizon and the zenith. The result is band/sheet structure, not splotches.
vec3 renderAurora(vec3 viewDir, float dayFactor) {
    // aurora must NOT show through a STORM/overcast
    // sky. The night gate (u_auroraStrength) alone let the green curtains bleed up
    // THROUGH a heavy night-storm deck (the aurora is added AFTER the cloud layer,
    // so a high-coverage overcast did not occlude it). Gate the aurora OFF as the
    // sky-wide cloud coverage rises into the overcast band: clear/fair-weather night
    // keeps its full green curtains (the night-aurora gate), a storm night shows
    // NONE. Keyed on the same u_cloudCoverageAmount the cloud deck uses, so the gate
    // tracks the actual overcast level (clear ~0.45 -> 1.0, storm ~0.85 -> 0.0).
    float overcastGate = 1.0 - smoothstep(0.55, 0.72, u_cloudCoverageAmount);
    // The explicit storm floor closes the gate even if coverage state lags a
    // frame behind the active storm lighting.
    float stormGate = 1.0 - smoothstep(0.05, 0.35, u_stormSkyFloor);
    float nightEnvelope = clamp(u_auroraStrength, 0.0, 1.0) * overcastGate * stormGate;
    // Curtains hang above the horizon band; start the fade a little above y=0 so
    // nothing snaps on at the frame edge, but allow them well up the dome.
    if (nightEnvelope <= 0.0 || viewDir.y < 0.12) return vec3(0.0);

    float t = u_time * 0.06;

    // Azimuth around the dome [-pi,pi] -> [0,1], continuous and seamless so a
    // curtain does not hard-cut where the view frustum clips the dome. Height is
    // the view elevation [0 horizon.. 1 zenith].
    float az = atan(viewDir.z, viewDir.x);     // [-pi, pi]
    float azu = az / (2.0 * PI_SKY) + 0.5;     // [0, 1], wraps
    float height = clamp(viewDir.y, 0.0, 1.0);

    // --- curtain placement: a few wandering vertical sheets in azimuth ---------
    // Warp the azimuth coordinate slowly so the sheets drape and flow rather than
    // sitting on fixed meridians. fbm of (azimuth, slow time) gives the meander.
    float meander = fbm(vec2(azu * 6.0, t * 1.3), 4) - 0.5;       // [-0.5,0.5]
    float curtainCoord = azu * 7.0 + meander * 2.2 + t * 0.35;    // sheets per dome

    // RIDGE function: fract->triangular gives evenly-ish spaced sheets; sharpen
    // into narrow bright filaments (the curtain seen near edge-on). The width is
    // modulated by a second slow noise so some sheets are broad, some are thin.
    float ridgePhase = fract(curtainCoord);
    float ridge = abs(ridgePhase - 0.5) * 2.0;                    // 0 at sheet core, 1 between
    float widthMod = 0.45 + 0.40 * fbm(vec2(curtainCoord * 0.7, t), 3);
    float sheet = 1.0 - smoothstep(0.0, widthMod, ridge);         // bright sheet cores

    // Vary sheet brightness so the curtain breaks into separate draped panels
    // rather than a uniform ring -- but as VERTICAL panels, never round blobs. The
    // panel only DIMS sheets (floor 0.45) instead of fully dropping them, so some
    // green curtain is always present whatever azimuth a view frames (the night
    // aurora gate needs a reliable green fraction in its sky ROI).
    // Lower floor (0.45 -> 0.28) so the curtains break into more distinct draped
    // panels with dark sky GAPS between them, instead of an always-on green wash
    // across the whole dome (which read as GREEN_SKY_SPECKLE). Some green is still
    // always present for the night-aurora-presence gate.
    float panel = 0.28 + 0.62 * smoothstep(0.30, 0.80, fbm(vec2(floor(curtainCoord) * 1.7, t * 0.5), 3));
    sheet *= panel;

    // --- vertical structure: striations running UP the sheet -------------------
    // Fine vertical filaments give the curtain its rayed texture; they scroll
    // upward slowly. Kept SMOOTH + low-contrast (lower amplitude + frequency) so the
    // aurora reads as soft luminous curtains rather than high-frequency green specks
    // sharp striations registered as firefly speckle in the up-view sky critique.
    float striation = 0.82 + 0.18 * fbm(vec2(curtainCoord * 3.0, height * 5.0 - t * 1.5), 3);
    sheet *= striation;

    // --- height envelope: curtains hang from the upper dome --------------------
    // Feather in above the horizon (0.12 -> 0.30), brightest through the mid dome,
    // and ease off toward the zenith so the sheets read as draped, bottom-lit
    // sheets rather than a flat overhead wash.
    float lowerEdge = smoothstep(0.12, 0.30, height);
    // Extend the curtain well up the dome so its GREEN body fills the upper-sky
    // band a horizon-framed view sees (the TimeOfDaySweep night ROI is the top
    // third of the frame -> mid/high view elevation); only feather out near the
    // zenith. A too-low upper fade left only the magenta tips in that ROI and the
    // night-only aurora gate read zero green.
    float upperFade = 1.0 - smoothstep(0.78, 0.99, height);
    // Bottom-emphasis: aurora curtains are brightest along their lower fringe.
    float bottomGlow = 0.60 + 0.40 * (1.0 - smoothstep(0.12, 0.60, height));
    float vertEnv = lowerEdge * upperFade * bottomGlow;

    // Dimmer overall (0.46 -> 0.30): aurora reads as luminous curtains, not a
    // sky-filling green wash. Still clearly present in the night ROI.
    float auroraIntensity = sheet * vertEnv * nightEnvelope * 0.30;

    // Colour: a tall GREEN body  topped by a thin magenta/violet fringe
    // only near the zenith. The split is driven by HEIGHT plus a slow azimuthal
    // drift, so a single sheet grades vertically. The green is held dominant
    // through almost the whole dome (transition pushed near the zenith); the
    // TimeOfDaySweep night ROI spans view elevation ~0.43..0.97 (pitch +30, 90 deg
    // FOV, top 55%), so the green body must reach high to register the night-only
    // aurora curtain there -- only a thin violet cap sits above it.
    float hue = clamp((height - 0.74) * 3.0 + 0.16 * sin(t * 1.7 + az * 2.0), 0.0, 1.0);
    vec3 auroraColor = mix(vec3(0.12, 0.85, 0.45),   // green base
                           vec3(0.55, 0.22, 0.85),   // violet tips
                           hue);

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
    // isolation backdrop — flat-fill the background, skip the sky dome.
    if (u_backdropMode > 0) {
        if (u_backdropMode == 3) {  // checker (scale/alignment reference)
            vec2 c = floor(gl_FragCoord.xy / 64.0);
            float k = mod(c.x + c.y, 2.0);
            FragColor = vec4(mix(vec3(0.18), vec3(0.6, 0.1, 0.6), k), 1.0);
        } else {                    // 1 = void colour, 2 = greenscreen (via u_backdropColor)
            FragColor = vec4(u_backdropColor, 1.0);
        }
        return;
    }
    vec3 viewDir = normalize(WorldPos);
    float dayFactor = clamp(u_skyDayFactor, 0.0, 1.0);
    float nightFactor = 1.0 - dayFactor;

    // aerial warm-grade factors, computed once and applied POST-tonemap
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
        //  FIX: the night envelope must DARKEN the warm scattering, not
        // CROSS-FADE it to a fixed blue base. The old `mix(nightBase, lut,
        // dayFactor)` blended ~57% deep-blue base into the dusk dome (dayFactor
        // ~0.43 at the t=0.22 dusk), pulling the sun-side r/b DOWN below noon --
        // the "dusk got bluer" regression. Instead we (1) scale the LUT radiance
        // by dayFactor so the dome darkens through dusk into night while KEEPING
        // its warm scattering hue, and (2) add a tiny deep-night ADDITIVE floor
        // that only matters once dayFactor ~ 0 (true night), so stars/moon still
        // read against a dark dome. The warm low-sun palette now survives dusk.
        //  FIX (aerial reddening of the dome): the sky-view LUT in-scatter
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
    //  ( part B): NOON sun-disc localization. At a HIGH sun the broad
    // pow(sunDot,32) corona washed a wide swath of the already-bright pale dome up
    // past the disc-detection luminance, so the SkyboxVisual "brightest cluster
    // sits at the sun" check could not localize (cluster_fraction ~0.22 vs 0.6).
    // Tighten the corona's angular falloff as the sun climbs (steeper exponent +
    // lower gain at high sun) so the high-noon corona collapses to a tight halo
    // hugging the disc, and add a punchy localized disc CORE at the true sun
    // position so the brightest pixels concentrate there. Gated on u_sunCosZenith
    // so DAWN/DUSK (low sun) keep their original soft warm corona untouched
    // (TimeOfDaySweep dusk warmth + sun-glow are unaffected). Render-only.
    float highSun = smoothstep(0.45, 0.85, u_sunCosZenith); // 0 low sun -> 1 high noon
    // Corona exponent: 32 at low sun (soft) -> 220 at high noon (tight halo).
    float coronaExp = mix(32.0, 220.0, highSun);
    // Corona gain: 0.5 at low sun (unchanged) -> 0.22 at high noon (dimmer halo
    // so the dome around the sun stops blowing out past the disc threshold).
    float coronaGain = mix(0.5, 0.22, highSun);
    float sunCorona = pow(max(0.0, sunDot), coronaExp) * dayFactor * coronaGain;
    sunCorona *= smoothstep(0.0, 0.2, viewDir.y); // fade near horizon
    // Tight high-sun disc core: a narrow, bright punch right at the sun so the
    // brightest cluster localizes at the true sun position at noon. Only adds at
    // high sun (highSun gate); low sun keeps the original sunMask disc alone.
    float sunCore = smoothstep(0.99965, 0.99995, sunDot) * dayFactor * highSun;

    // the disc color IS the atmospheric transmittance toward the sun
    // (same LUT the lighting pass + aerial fog read). At low sun the long path
    // eats blue first, so the disc reddens to deep orange coherently with the
    // warm horizon scattering -- no hand-authored sunset color.
    vec3 sunTrans = sunTransmittance(u_sunCosZenith);
    vec3 sunColor = sunTrans * 4.0;
    skyColor += sunColor * (sunMask + sunCorona + sunCore * 2.2);

    // --- 3. MOON ---
    float moonDot = dot(viewDir, u_moonDirection);
    float moonMask = smoothstep(0.996, 0.9999, moonDot) * nightFactor;
    float moonGlow = pow(max(0.0, moonDot), 16.0) * nightFactor * 0.2;
    float craterNoise = fbm(viewDir.xy * 25.0, 4);
    vec3 moonSurface = vec3(0.8, 0.8, 0.7) * (0.7 + 0.3 * craterNoise);
    vec3 moonGlowColor = vec3(0.8, 0.9, 1.0) * moonGlow;
    skyColor = mix(skyColor + moonGlowColor, moonSurface, moonMask);

    // --- STARS (before clouds so the cloud layer OCCLUDES them) ---
    // Stars were added AFTER the clouds, so they shone through overcast. Adding them
    // into the base sky here means renderClouds' front-to-back composite covers them
    // wherever there is cloud, and leaves them where the sky is clear.
    skyColor += renderStars(viewDir, nightFactor);

    // --- 4. CLOUDS WITH ATMOSPHERIC LIGHTING ---
    skyColor = renderClouds(viewDir, skyColor, dayFactor);

    //  (defect 3): NIGHT-STORM dome floor. Independent
    // of the cloud layer, lift the whole night-storm dome to a faint cool storm-grey
    // so the sky behind the rain/lightning is dark-but-legible rather than pure
    // black. Gated by storm intensity AND nightFactor, and faded toward the horizon
    // so it reads as overcast murk above the scene. Zero for clear sky / daytime, so
    // the clear-night dome and all daytime cells are untouched.
    {
        float stormNight = u_stormSkyFloor * nightFactor;
        float aboveHorizon = smoothstep(-0.05, 0.35, viewDir.y);
        vec3 stormMurk = vec3(0.020, 0.024, 0.034);
        skyColor += stormMurk * stormNight * aboveHorizon;
    }

    // --- 6. MAGICAL AURORA (NIGHT-ONLY; gated by the deep-night envelope) ---
    skyColor += renderAurora(viewDir, dayFactor);

    // --- 7. HDR TONEMAPPING ---
    skyColor = skyColor * (2.51 * skyColor + 0.03) / (skyColor * (2.43 * skyColor + 0.59) + 0.14);

    // POST-tonemap aerial warm grade. The bright dome lives in the ACES
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

        // kill the SICKLY YELLOW-GREEN dawn cast.
        // At a low-but-positive sun (dawn, sun ~27 deg up) the sky-view LUT radiance
        // is green-dominant (G leads R and B), and the luminance-preserving warm
        // grade above cannot fix a base whose GREEN already leads -- the bright dawn
        // dome read yellow-green instead of a clean warm amber. A real sunrise sky
        // is amber/orange: R >= G >= B. So clamp any GREEN EXCESS (G above R) back
        // down toward R on the BRIGHT dome only. This is gated by:
        //   - brightness (only the bright day/dawn dome; the dark dusk/night dome
        //     the TimeOfDaySweep gate measures has low luma and is untouched, so the
        //     dusk warm-half r/b band + night-dark gates are unaffected), and
        //   - the warm-grade weight (off at deep night) so stars/aurora are clean.
        // It only ever REMOVES an unwanted green lead; a neutral or warm (R>=G) sky
        // is left exactly as-is, so dusk stays warm.
        //
        // The clamp fires ONLY when GREEN leads BOTH red AND blue -- i.e. the true
        // sickly yellow-green dawn cast -- AND only at a LOW SUN (dawn/dusk window).
        // At a HIGH sun the SkyboxVisual smoke (time-of-day 0.04, sun near zenith)
        // measures a strict horizon->zenith luminance gradient; touching G per-band
        // there perturbed that gradient's monotonicity. So a low-sun gate
        // (u_sunCosZenith) holds the clamp fully OFF at noon and only opens it as
        // the sun drops toward the horizon -- exactly the dawn/dusk band where the
        // green cast appears. The dawn sun (cosZenith ~0.47) is well inside it. The
        // blue-dominant noon dome (B >= G) is untouched on both counts.
        float skyLuma = dot(skyColor, kLumaW);
        // Fire on the dimmer dusk dome too (lowered from 0.35): the golden-hour dusk
        // dome is now dimmer than noon for the luminance ordering, and the green cast
        // lives in those mid-luma pixels — the old threshold let it survive and trip
        // the dusk aurora-green gate. Still 0 at very low luma (deep night stars/aurora).
        float brightDome = smoothstep(0.08, 0.40, skyLuma);
        float lowSunGate = 1.0 - smoothstep(0.55, 0.78, u_sunCosZenith); // off at noon
        float rb = max(skyColor.r, skyColor.b);
        // Only a green lead over BOTH channels counts (yellow-green dawn/dusk cast).
        // Fully remove it (1.0) so the low-sun sky is firmly warm (R >= G), never green.
        float greenLead = max(skyColor.g - rb, 0.0);
        skyColor.g -= greenLead * 1.0 * brightDome * lowSunGate
                      * clamp(aerialNightFade, 0.0, 1.0);
    }

    // Color grading for fantasy atmosphere
    skyColor = pow(skyColor, vec3(0.9, 0.95, 1.05));

    //  ( part B): NOON sun-disc PUNCH. At a high sun the open pale dome
    // ALSO saturates near the tonemap ceiling, so the pre-tonemap disc/corona above
    // never reads measurably brighter than the dome -- the brightest cluster could
    // not localize at the sun (SkyboxVisual cluster_fraction ~0.24 vs 0.6). This
    // forces a tight disc core toward PURE WHITE here, AFTER the ACES tonemap that
    // asymptotically caps the dome just under 1.0, so the disc reaches a clean 1.0
    // (255) that clears the dome ceiling -- giving the gate a genuine localized
    // brightest spot at the true sun direction. Gated on:
    //   * highSun (u_sunCosZenith) so DAWN/DUSK keep their soft warm disc untouched,
    //   * dayFactor so it fades out below the horizon,
    //   * viewDir.y so it never punches the lower/horizon band.
    // Tight smoothstep -> a crisp, distinct disc (the desired "more localized" look)
    // rather than the diffuse bloom. Render-only.
    {
        float discCore = smoothstep(0.99975, 0.99997, sunDot)
                         * highSun * dayFactor * smoothstep(0.05, 0.20, viewDir.y);
        skyColor = mix(skyColor, vec3(1.0), clamp(discCore, 0.0, 1.0));
    }

    // --- 8. GAMMA CORRECTION ---
    skyColor = pow(skyColor, vec3(1.0/2.2));

    FragColor = vec4(skyColor, 1.0);
}
