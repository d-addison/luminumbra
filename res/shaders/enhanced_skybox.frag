#version 450 core
out vec4 FragColor;

in vec3 WorldPos;

// Enhanced atmospheric uniforms
uniform vec3 u_sunDirection;
uniform vec3 u_moonDirection;
uniform float u_sunIntensity;
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
vec3 atmosphericScattering(vec3 viewDir, vec3 sunDir, float sunIntensity) {
    float cosTheta = dot(viewDir, sunDir);
    float scattering = pow(max(0.0, cosTheta), 8.0) * sunIntensity;

    // Rayleigh scattering (blue haze). Weighted toward the horizon where the
    // optical path through the atmosphere is longest; a flat additive term
    // washed the whole sky toward white and erased the horizon->zenith
    // luminance gradient (caught by skybox_visual_smoke).
    float horizonFactor = pow(1.0 - clamp(viewDir.y, 0.0, 1.0), 2.0);
    vec3 rayleigh = vec3(0.3, 0.6, 1.0) * (1.0 - sunIntensity * 0.5) * (0.15 + 0.85 * horizonFactor);

    // Mie scattering (sun glow)
    vec3 mie = vec3(1.0, 0.8, 0.6) * scattering * 0.5;

    return rayleigh + mie;
}

// Enhanced cloud rendering
vec3 renderClouds(vec3 viewDir, vec3 baseColor) {
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
        1.0 - u_sunIntensity
    );
    
    // Apply atmospheric lighting to clouds
    float sunDot = dot(viewDir, u_sunDirection);
    float cloudLighting = max(0.3, sunDot * 0.5 + 0.5);
    cloudColor *= cloudLighting * u_sunIntensity + 0.2; // Ambient contribution
    
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
    float nightFactor = 1.0 - u_sunIntensity;
    
    // --- 1. BASE ATMOSPHERIC GRADIENT ---
    // Deeper zenith blue: the daytime sky must stay measurably darker at the
    // zenith than at the horizon (horizon-gradient gate).
    vec3 dayTopColor = vec3(0.22, 0.45, 0.92) * u_skyTint;
    vec3 dayBottomColor = vec3(0.9, 0.95, 1.0) * u_skyTint;
    
    // Sunset/sunrise colors
    vec3 sunsetTop = vec3(0.8, 0.4, 0.2);
    vec3 sunsetBottom = vec3(1.0, 0.6, 0.3);
    
    vec3 nightTop = vec3(0.01, 0.02, 0.08);
    vec3 nightBottom = vec3(0.05, 0.1, 0.25);
    
    // Blend based on sun intensity
    float sunsetFactor = pow(max(0.0, 1.0 - abs(u_sunIntensity - 0.3) / 0.3), 2.0);
    
    vec3 topColor = mix(
        mix(nightTop, dayTopColor, u_sunIntensity),
        sunsetTop,
        sunsetFactor
    );
    
    vec3 bottomColor = mix(
        mix(nightBottom, dayBottomColor, u_sunIntensity),
        sunsetBottom,
        sunsetFactor
    );
    
    // Vertical gradient with atmospheric curve
    float horizonBlend = smoothstep(-0.2, 0.6, viewDir.y);
    vec3 skyColor = mix(bottomColor, topColor, horizonBlend);
    
    // --- 2. ATMOSPHERIC SCATTERING ---
    vec3 scatterColor = atmosphericScattering(viewDir, u_sunDirection, u_sunIntensity);
    skyColor += scatterColor * u_atmosDensity;
    
    // --- 3. ENHANCED SUN DISC ---
    float sunDot = dot(viewDir, u_sunDirection);
    float sunMask = smoothstep(0.995, 0.9999, sunDot) * u_sunIntensity;
    
    // Sun corona effect
    float sunCorona = pow(max(0.0, sunDot), 32.0) * u_sunIntensity * 0.5;
    sunCorona *= smoothstep(0.0, 0.2, viewDir.y); // Fade near horizon
    
    vec3 sunColor = vec3(1.0, 0.9, 0.7) * 3.0;
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
    skyColor = renderClouds(viewDir, skyColor);
    
    // --- 6. STARS ---
    skyColor += renderStars(viewDir, nightFactor);
    
    // --- 7. MAGICAL AURORA ---
    skyColor += renderAurora(viewDir, nightFactor);
    
    // --- 8. ATMOSPHERIC PERSPECTIVE ---
    // Add depth and atmosphere
    float atmosphereGlow = pow(max(0.0, viewDir.y), 0.3) * u_atmosDensity;
    skyColor += vec3(0.1, 0.15, 0.3) * atmosphereGlow * nightFactor;
    
    // --- 9. ENHANCED HDR TONEMAPPING ---
    // Filmic tonemapping for cinematic look
    skyColor = skyColor * (2.51 * skyColor + 0.03) / (skyColor * (2.43 * skyColor + 0.59) + 0.14);
    
    // Color grading for fantasy atmosphere
    skyColor = pow(skyColor, vec3(0.9, 0.95, 1.05)); // Slight color shift
    
    // --- 10. GAMMA CORRECTION ---
    skyColor = pow(skyColor, vec3(1.0/2.2));
    
    FragColor = vec4(skyColor, 1.0);
}