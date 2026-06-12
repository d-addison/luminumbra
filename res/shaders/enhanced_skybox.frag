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

// Atmospheric scattering approximation
// T-I4-DR-tod-sky-balance: dayFactor (1 day -> 0 night) gates the lit haze.
// The rayleigh blue-haze used to scale by (1 - sunIntensity*0.5), so at night
// (sunIntensity 0) it sat at FULL strength and lit the whole dome blue - the
// dome could not go dark. Scattering is daylight illuminating the air, so it
// must fade out as the sun sets; gate both rayleigh and mie by dayFactor.
vec3 atmosphericScattering(vec3 viewDir, vec3 sunDir, float dayFactor) {
    float cosTheta = dot(viewDir, sunDir);
    float scattering = pow(max(0.0, cosTheta), 8.0) * dayFactor;

    // Rayleigh scattering (blue haze). Weighted toward the horizon where the
    // optical path through the atmosphere is longest; a flat additive term
    // washed the whole sky toward white and erased the horizon->zenith
    // luminance gradient (caught by skybox_visual_smoke).
    float horizonFactor = pow(1.0 - clamp(viewDir.y, 0.0, 1.0), 2.0);
    vec3 rayleigh = vec3(0.3, 0.6, 1.0) * dayFactor * (0.15 + 0.85 * horizonFactor);

    // Mie scattering (sun glow)
    vec3 mie = vec3(1.0, 0.8, 0.6) * scattering * 0.5;

    return rayleigh + mie;
}

// Enhanced cloud rendering
// T-I4-DR-tod-sky-balance: dayFactor lights the clouds. At night they fall to a
// faint dark silhouette instead of holding a lit sunset tint over the dome.
vec3 renderClouds(vec3 viewDir, vec3 baseColor, float dayFactor) {
    if(viewDir.y < 0.0) return baseColor; // No clouds below horizon
    
    // Multiple cloud layers for depth
    float cloudTime = u_time * 0.02;
    vec2 cloudUV = viewDir.xz / (viewDir.y + 0.5) * 0.5;
    
    // High altitude wispy clouds (weight reduced so the zenith keeps its
    // deep-blue gradient instead of whitewashing under cloud cover)
    float highClouds = fbm(cloudUV * 2.0 + vec2(cloudTime, cloudTime * 0.7), 4);
    highClouds = smoothstep(0.4, 0.8, highClouds) * 0.45;
    
    // Mid altitude puffy clouds
    float midClouds = fbm(cloudUV * 1.0 + vec2(cloudTime * 0.5, -cloudTime * 0.3), 5);
    midClouds = smoothstep(0.5, 0.9, midClouds) * u_cloudCoverage;
    
    // Low altitude atmospheric haze
    float haze = fbm(cloudUV * 0.5 + vec2(-cloudTime * 0.2, cloudTime * 0.1), 3);
    haze = smoothstep(0.3, 0.7, haze) * 0.3;
    
    // Cloud coloring based on sun position and time of day
    vec3 cloudColor = mix(
        vec3(0.9, 0.9, 1.0),  // Day clouds
        vec3(0.6, 0.4, 0.8),  // Sunset/sunrise clouds
        1.0 - dayFactor
    );

    // Apply atmospheric lighting to clouds. T-I4-DR-tod-sky-balance: the
    // ambient floor now fades with dayFactor so night clouds do not self-light
    // the dome (was a flat +0.2 that lit clouds even with the sun down).
    float sunDot = dot(viewDir, u_sunDirection);
    float cloudLighting = max(0.3, sunDot * 0.5 + 0.5);
    cloudColor *= cloudLighting * dayFactor + 0.04 * dayFactor; // Ambient contribution
    
    // Combine cloud layers
    float totalCloudDensity = clamp(highClouds + midClouds + haze, 0.0, 1.0);
    return mix(baseColor, cloudColor, totalCloudDensity);
}

// Enhanced star field
vec3 renderStars(vec3 viewDir, float nightIntensity) {
    if(nightIntensity < 0.1) return vec3(0.0);
    
    // Multiple star layers for richness
    float stars1 = noise(viewDir.xy * 800.0);
    float stars2 = noise(viewDir.xz * 1200.0 + vec2(100.0, 200.0));
    float stars3 = noise(viewDir.yz * 600.0 + vec2(300.0, 400.0));
    
    // Different star sizes and intensities
    float brightStars = smoothstep(0.99, 1.0, stars1);
    float mediumStars = smoothstep(0.985, 0.995, stars2) * 0.7;
    float dimStars = smoothstep(0.97, 0.98, stars3) * 0.4;
    
    // Twinkling effect
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
    
    // Flowing aurora patterns
    float aurora1 = sin(auroraUV.x * 2.0 + auroraTime) * cos(auroraUV.y + auroraTime * 0.7);
    float aurora2 = sin(auroraUV.x * 1.5 - auroraTime * 0.8) * cos(auroraUV.y * 1.3 - auroraTime);
    
    // Noise for organic movement
    float auroraFlow = fbm(auroraUV + vec2(auroraTime, -auroraTime * 0.5), 3);
    
    float auroraIntensity = (aurora1 + aurora2) * auroraFlow;
    auroraIntensity = smoothstep(0.2, 0.8, abs(auroraIntensity)) * nightIntensity * 0.3;
    
    // Magical aurora colors
    vec3 auroraColor = mix(
        vec3(0.2, 0.8, 0.4),  // Green
        vec3(0.6, 0.2, 0.9),  // Purple
        sin(auroraTime + viewDir.x * 5.0) * 0.5 + 0.5
    );
    
    return auroraColor * auroraIntensity;
}

void main()
{
    vec3 viewDir = normalize(WorldPos);
    // T-I4-DR-tod-sky-balance: the dome's time-of-day blend now rides the
    // continuous sun-elevation factor. nightFactor (stars/moon/aurora and the
    // sub-horizon dome) follows it so the dome darkens in lockstep with terrain
    // lighting instead of clinging to the clamped u_sunIntensity.
    float dayFactor = clamp(u_skyDayFactor, 0.0, 1.0);
    float nightFactor = 1.0 - dayFactor;

    // --- 1. BASE ATMOSPHERIC GRADIENT ---
    // Deeper zenith blue: the daytime sky must stay measurably darker at the
    // zenith than at the horizon (horizon-gradient gate).
    vec3 dayTopColor = vec3(0.22, 0.45, 0.92) * u_skyTint;
    vec3 dayBottomColor = vec3(0.9, 0.95, 1.0) * u_skyTint;

    // Sunset/sunrise colors
    vec3 sunsetTop = vec3(0.8, 0.4, 0.2);
    vec3 sunsetBottom = vec3(1.0, 0.6, 0.3);

    // T-I4-DR-tod-sky-balance: a true dark night dome. The previous nightBottom
    // (0.05,0.1,0.25) plus the lit haze/glow below held the night dome at ~182
    // sky luma even with the ground near-black; these deep values let the dome
    // tonemap down to a dark night sky so emissive/moon/star point features
    // read against it.
    vec3 nightTop = vec3(0.002, 0.004, 0.012);
    vec3 nightBottom = vec3(0.006, 0.012, 0.03);

    // Twilight warm band, keyed off the sun elevation (peaks while the sun sits
    // low, ~dayFactor 0.35). The sunset tint is concentrated toward the sun's
    // azimuth so the warm side of the dome reads R>B, matching a real sunset
    // instead of tinting the whole sphere.
    float sunsetFactor = pow(max(0.0, 1.0 - abs(dayFactor - 0.35) / 0.35), 2.0);
    float sunDot = dot(viewDir, u_sunDirection);
    float sunSide = smoothstep(-0.1, 0.85, sunDot);   // 1 toward the sun, 0 away
    float warmBand = sunsetFactor * (0.35 + 0.65 * sunSide);

    vec3 topColor = mix(
        mix(nightTop, dayTopColor, dayFactor),
        sunsetTop,
        warmBand
    );

    vec3 bottomColor = mix(
        mix(nightBottom, dayBottomColor, dayFactor),
        sunsetBottom,
        warmBand
    );

    // Vertical gradient with atmospheric curve
    float horizonBlend = smoothstep(-0.2, 0.6, viewDir.y);
    vec3 skyColor = mix(bottomColor, topColor, horizonBlend);

    // --- 2. ATMOSPHERIC SCATTERING ---
    // Gated by dayFactor (see atmosphericScattering): daylight scattering fades
    // out as the sun sets so it no longer floods the night dome.
    vec3 scatterColor = atmosphericScattering(viewDir, u_sunDirection, dayFactor);
    skyColor += scatterColor * u_atmosDensity;

    // --- 3. ENHANCED SUN DISC ---
    // T-I4-DR-tod-sky-balance: disc/corona ride dayFactor, not the clamped
    // u_sunIntensity. A low dusk sun (u_sunIntensity still 1) now renders a
    // softer warm disc, and below the horizon the disc fades out entirely.
    float sunMask = smoothstep(0.995, 0.9999, sunDot) * dayFactor;

    // Sun corona effect
    float sunCorona = pow(max(0.0, sunDot), 32.0) * dayFactor * 0.5;
    sunCorona *= smoothstep(0.0, 0.2, viewDir.y); // Fade near horizon

    // Warm the disc toward the horizon so the dusk sun glows orange.
    vec3 sunColor = mix(vec3(1.0, 0.55, 0.25), vec3(1.0, 0.9, 0.7), dayFactor) * 3.0;
    skyColor += sunColor * (sunMask + sunCorona);
    
    // --- 4. ENHANCED MOON ---
    float moonDot = dot(viewDir, u_moonDirection);
    float moonMask = smoothstep(0.996, 0.9999, moonDot) * nightFactor;
    
    // Moon glow
    float moonGlow = pow(max(0.0, moonDot), 16.0) * nightFactor * 0.2;
    
    // Moon surface with craters
    float craterNoise = fbm(viewDir.xy * 25.0, 4);
    vec3 moonSurface = vec3(0.8, 0.8, 0.7) * (0.7 + 0.3 * craterNoise);
    vec3 moonGlowColor = vec3(0.8, 0.9, 1.0) * moonGlow;
    
    skyColor = mix(skyColor + moonGlowColor, moonSurface, moonMask);
    
    // --- 5. CLOUDS WITH ATMOSPHERIC LIGHTING ---
    skyColor = renderClouds(viewDir, skyColor, dayFactor);
    
    // --- 6. STARS ---
    skyColor += renderStars(viewDir, nightFactor);
    
    // --- 7. MAGICAL AURORA ---
    skyColor += renderAurora(viewDir, nightFactor);
    
    // --- 8. ATMOSPHERIC PERSPECTIVE ---
    // T-I4-DR-tod-sky-balance: this zenith haze used to scale by nightFactor,
    // so it RAN ONLY AT NIGHT and was a primary reason the night dome stayed
    // bright blue (~182 sky luma). It is a daylight scattering effect, so gate
    // it by dayFactor: a subtle lift during the day, nothing at night.
    float atmosphereGlow = pow(max(0.0, viewDir.y), 0.3) * u_atmosDensity;
    skyColor += vec3(0.1, 0.15, 0.3) * atmosphereGlow * dayFactor;
    
    // --- 9. ENHANCED HDR TONEMAPPING ---
    // Filmic tonemapping for cinematic look
    skyColor = skyColor * (2.51 * skyColor + 0.03) / (skyColor * (2.43 * skyColor + 0.59) + 0.14);
    
    // Color grading for fantasy atmosphere
    skyColor = pow(skyColor, vec3(0.9, 0.95, 1.05)); // Slight color shift
    
    // --- 10. GAMMA CORRECTION ---
    skyColor = pow(skyColor, vec3(1.0/2.2));
    
    FragColor = vec4(skyColor, 1.0);
}