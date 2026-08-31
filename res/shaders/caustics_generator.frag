#version 450 core
out vec4 FragColor;

uniform float u_time;
uniform vec2 u_resolution = vec2(512.0, 512.0);

// Caustics generation using  patterns
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = hash(i + vec2(0.0, 0.0));
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Generate realistic water surface height using multiple
float waterHeight(vec2 uv, float time) {
    float height = 0.0;

    // Large
    height += sin(uv.x * 2.0 + time * 0.5) * cos(uv.y * 1.8 + time * 0.7) * 0.4;

    // Medium
    height += sin(uv.x * 4.0 - time * 0.8) * cos(uv.y * 3.2 + time * 0.6) * 0.2;

    // Small ripples
    height += sin(uv.x * 8.0 + time * 1.2) * cos(uv.y * 6.5 - time * 0.9) * 0.1;

    // Wind-driven
    height += sin(uv.x * 1.5 + uv.y * 0.8 + time * 0.4) * 0.15;

    // Add noise for organic variation
    height += (noise(uv * 3.0 + vec2(time * 0.1, time * 0.15)) - 0.5) * 0.3;

    return height;
}

// Calculate water surface normal
vec3 waterNormal(vec2 uv, float time) {
    float eps = 0.01;

    float heightL = waterHeight(uv - vec2(eps, 0.0), time);
    float heightR = waterHeight(uv + vec2(eps, 0.0), time);
    float heightD = waterHeight(uv - vec2(0.0, eps), time);
    float heightU = waterHeight(uv + vec2(0.0, eps), time);

    vec3 normal = normalize(vec3(
        (heightL - heightR) / (2.0 * eps),
        (heightD - heightU) / (2.0 * eps),
        1.0
    ));

    return normal;
}

// Simulate light refraction through water surface
float causticIntensity(vec2 uv, float time) {
    // Multiple light ray simulation points for realistic caustics
    float intensity = 0.0;

    // Simulate light rays hitting the water surface
    for(int i = 0; i < 4; i++) {
        for(int j = 0; j < 4; j++) {
            vec2 offset = vec2(float(i) - 1.5, float(j) - 1.5) * 0.1;
            vec2 sampleUV = uv + offset;

            // Get surface normal at this point
            vec3 normal = waterNormal(sampleUV, time);

            // Light direction (from above)
            vec3 lightDir = vec3(0.2, 0.3, -1.0);
            lightDir = normalize(lightDir);

            // Calculate refraction
            float ior = 1.33; // Water index of refraction
            vec3 refracted = refract(lightDir, normal, 1.0 / ior);

            // Calculate where refracted ray hits the bottom
            float waterDepth = 2.0; // Assumed water depth
            if(refracted.z != 0.0) {
                vec2 bottomHit = sampleUV + (refracted.xy / -refracted.z) * waterDepth;

                // Distance from current UV to where light ray hits
                float dist = length(bottomHit - uv);

                // Add intensity based on ray convergence
                float rayIntensity = 1.0 / (1.0 + dist * dist * 20.0);

                // Modulate by surface-wave strength for realistic variation.
                float waveStrength = length(normal.xy) * 2.0;
                rayIntensity *= (1.0 + waveStrength);

                intensity += rayIntensity;
            }
        }
    }

    return intensity;
}

// Add animated caustic patterns
float animatedCaustics(vec2 uv, float time) {
    // Base caustic pattern
    float caustics = causticIntensity(uv, time);

    // Add flowing animation
    vec2 flow1 = vec2(cos(time * 0.1), sin(time * 0.15)) * 0.1;
    vec2 flow2 = vec2(sin(time * 0.08), cos(time * 0.12)) * 0.15;

    float caustics1 = causticIntensity(uv + flow1, time + 0.5);
    float caustics2 = causticIntensity(uv + flow2, time - 0.3);

    // Combine multiple caustic layers
    caustics = max(caustics, caustics1 * 0.8);
    caustics = max(caustics, caustics2 * 0.6);

    // Add some high-frequency detail
    float detail = noise(uv * 20.0 + time * 0.5) * 0.1;
    caustics += detail;

    return caustics;
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;

    // Generate caustic pattern
    float caustics = animatedCaustics(uv, u_time);

    // Apply some contrast and brightness adjustment
    caustics = pow(caustics * 0.8, 1.5);
    caustics = clamp(caustics, 0.0, 1.0);

    // Add some color variation for more realistic caustics
    vec3 causticColor = vec3(caustics);

    // Slight blue-green tint
    causticColor.g *= 1.1;
    causticColor.b *= 0.95;

    // Add some brightness variation
    float brightness = 0.8 + 0.4 * noise(uv * 5.0 + u_time * 0.2);
    causticColor *= brightness;

    FragColor = vec4(causticColor, caustics);
}
