#version 450 core
out vec4 FragColor;

in vec3 WorldPos;

uniform vec3 u_sunDirection;
uniform vec3 u_moonDirection;
uniform float u_sunIntensity;
uniform float u_time;

// Simple procedural noise functions for stars
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

void main()
{
    vec3 viewDir = normalize(WorldPos);
    float nightFactor = 1.0 - u_sunIntensity;

    // --- 1. SKY GRADIENT ---
    vec3 dayTop = vec3(0.5, 0.7, 1.0);
    vec3 dayBottom = vec3(0.95, 0.95, 1.0);
    vec3 nightTop = vec3(0.01, 0.02, 0.05);
    vec3 nightBottom = vec3(0.05, 0.1, 0.2);

    vec3 topColor = mix(nightTop, dayTop, u_sunIntensity);
    vec3 bottomColor = mix(nightBottom, dayBottom, u_sunIntensity);

    float t = smoothstep(-0.1, 0.4, viewDir.y);
    vec3 skyColor = mix(bottomColor, topColor, t);

    // --- 2. STARS ---
    float star_noise = noise(viewDir.xz * 1000.0);
    float stars = smoothstep(0.99, 1.0, star_noise);
    stars *= 0.5 + 0.5 * sin(u_time * 2.0 + viewDir.x * 500.0);
    vec3 starColor = stars * nightFactor * vec3(0.9, 0.9, 1.0);

    // --- 3. SUN DISC ---
    float sunDot = dot(viewDir, u_sunDirection);
    float sunMask = smoothstep(0.995, 0.9999, sunDot) * u_sunIntensity;
    vec3 sunColor = vec3(1.0, 0.9, 0.7) * 2.0;

    // --- 4. MOON DISC ---
    float moonDot = dot(viewDir, u_moonDirection);
    float moonMask = smoothstep(0.996, 0.9999, moonDot) * nightFactor;
    float crater_noise = noise(viewDir.xy * 20.0);
    vec3 moonBaseColor = vec3(0.7, 0.7, 0.6);
    vec3 moonColor = mix(moonBaseColor, moonBaseColor * 0.6, crater_noise);

    // ===================== FIX STARTS HERE =====================

    // --- 5. COMPOSITION ---
    // Start with the base sky color and stars
    vec3 finalColor = skyColor + starColor;

    // ADD the sun's light on top of the sky. This makes it glow.
    finalColor += sunColor * sunMask;

    // MIX the moon's color. The moon is opaque and should block the sky/stars.
    finalColor = mix(finalColor, moonColor, moonMask);

    // --- 6. TONEMAPPING ---
    // Apply Reinhard tonemapping to compress the HDR colors (from the sun)
    // into the visible [0, 1] range, preventing it from looking washed out.
    // This is the same tonemapping from your lighting_pass shader.
    finalColor = finalColor / (finalColor + vec3(1.0));
    
    // Apply Gamma Correction
    finalColor = pow(finalColor, vec3(1.0/2.2));

    FragColor = vec4(finalColor, 1.0);
    // ====================== FIX ENDS HERE ======================
}