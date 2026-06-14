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

// Enhanced cloud rendering
// T-I4-DR-tod-sky-balance: dayFactor lights the clouds. At night they fall to a
// faint dark silhouette instead of holding a lit sunset tint over the dome.
vec3 renderClouds(vec3 viewDir, vec3 baseColor, float dayFactor) {
    if(viewDir.y < 0.0) return baseColor; // No clouds below horizon

    float cloudTime = u_time * 0.02;
    vec2 cloudUV = viewDir.xz / (viewDir.y + 0.5) * 0.5;

    float highClouds = fbm(cloudUV * 2.0 + vec2(cloudTime, cloudTime * 0.7), 4);
    highClouds = smoothstep(0.4, 0.8, highClouds) * 0.45;

    float midClouds = fbm(cloudUV * 1.0 + vec2(cloudTime * 0.5, -cloudTime * 0.3), 5);
    midClouds = smoothstep(0.5, 0.9, midClouds) * u_cloudCoverage;

    float haze = fbm(cloudUV * 0.5 + vec2(-cloudTime * 0.2, cloudTime * 0.1), 3);
    haze = smoothstep(0.3, 0.7, haze) * 0.3;

    vec3 cloudColor = mix(
        vec3(0.9, 0.9, 1.0),
        vec3(0.6, 0.4, 0.8),
        1.0 - dayFactor
    );

    float sunDot = dot(viewDir, u_sunDirection);
    float cloudLighting = max(0.3, sunDot * 0.5 + 0.5);
    cloudColor *= cloudLighting * dayFactor + 0.04 * dayFactor;

    float totalCloudDensity = clamp(highClouds + midClouds + haze, 0.0, 1.0);
    return mix(baseColor, cloudColor, totalCloudDensity);
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

    // --- 1. SCATTERING COLOR FROM THE SKY-VIEW LUT ---
    // The LUT supplies the COLOR (coherent low-sun pinks/oranges); u_skyDayFactor
    // supplies the night-darkening brightness envelope so the dome still goes
    // genuinely dark at night and stars/moon read against it.
    vec3 skyColor;
    if (u_useSkyLut != 0) {
        vec3 lutRadiance = sampleSkyView(viewDir) * u_skyExposure;
        // Night envelope: fade the lit scattering toward a deep night base as the
        // sun drops, matching the terrain/ambient elevation signal.
        vec3 nightBase = mix(vec3(0.006, 0.012, 0.03), vec3(0.002, 0.004, 0.012),
                             smoothstep(-0.2, 0.6, viewDir.y));
        skyColor = mix(nightBase, lutRadiance, dayFactor) * u_atmosDensity;
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

    // Color grading for fantasy atmosphere
    skyColor = pow(skyColor, vec3(0.9, 0.95, 1.05));

    // --- 8. GAMMA CORRECTION ---
    skyColor = pow(skyColor, vec3(1.0/2.2));

    FragColor = vec4(skyColor, 1.0);
}
