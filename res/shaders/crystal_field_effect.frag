#version 450 core

out vec4 FragColor;

in vec2 TexCoords;

// Scene inputs
uniform sampler2D u_sceneColor;
uniform sampler2D gPosition;
uniform sampler2D gNormal; 
uniform sampler2D gMaterial;
uniform sampler2D gDepth;

// Crystal field parameters
uniform vec3 u_cameraPos;
uniform float u_time;
uniform mat4 u_inverseView;
uniform mat4 u_inverseProjection;
uniform vec3 u_crystalPositions[16]; // Up to 16 crystal sources
uniform float u_crystalIntensities[16];
uniform int u_crystalCount;

// Field parameters
uniform float u_fieldStrength = 1.0;
uniform float u_fieldRadius = 50.0;
uniform vec3 u_fieldColor = vec3(0.3, 0.7, 1.0);

// Noise for energy field distortion
float hash(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float noise3D(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    
    return mix(
        mix(mix(hash(i + vec3(0,0,0)), hash(i + vec3(1,0,0)), f.x),
            mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
            mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y), f.z);
}

// Fractal noise for complex energy patterns
float fbm(vec3 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    
    for(int i = 0; i < octaves; i++) {
        value += amplitude * noise3D(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}

// Reconstruct world position from depth
vec3 worldPosFromDepth(vec2 uv, float depth) {
    float z = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 viewPos = u_inverseProjection * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos = u_inverseView * viewPos;
    return worldPos.xyz;
}

// Calculate crystal field effect at world position
vec3 calculateCrystalField(vec3 worldPos) {
    vec3 totalField = vec3(0.0);
    
    for(int i = 0; i < u_crystalCount && i < 16; i++) {
        vec3 crystalPos = u_crystalPositions[i];
        float crystalIntensity = u_crystalIntensities[i];
        
        float distance = length(worldPos - crystalPos);
        
        // Skip if too far from crystal
        if(distance > u_fieldRadius) continue;
        
        // Distance attenuation
        float attenuation = 1.0 - smoothstep(0.0, u_fieldRadius, distance);
        attenuation = pow(attenuation, 2.0); // Square falloff
        
        // Oscillating energy field
        float oscillation = sin(u_time * 2.0 + distance * 0.1) * 0.5 + 0.5;
        oscillation = mix(0.3, 1.0, oscillation);
        
        // Noise-based distortion for organic energy field
        vec3 noisePos = worldPos * 0.05 + vec3(u_time * 0.1, u_time * 0.15, u_time * 0.08);
        float energyNoise = fbm(noisePos, 3);
        energyNoise = energyNoise * 0.5 + 0.5; // Normalize
        
        // Direction-based field intensity (energy flows)
        vec3 fieldDir = normalize(worldPos - crystalPos);
        float directionFactor = abs(sin(atan(fieldDir.x, fieldDir.z) * 4.0 + u_time)) * 0.3 + 0.7;
        
        // Vertical energy column effect
        float heightFactor = exp(-abs(worldPos.y - crystalPos.y) * 0.1) * 2.0;
        
        // Combine all factors
        float fieldIntensity = attenuation * oscillation * energyNoise * directionFactor * heightFactor;
        fieldIntensity *= crystalIntensity * u_fieldStrength;
        
        // Color variation based on distance and noise
        vec3 fieldColor = u_fieldColor;
        fieldColor.r += sin(distance * 0.1 + u_time) * 0.2;
        fieldColor.g += cos(distance * 0.15 + u_time * 1.2) * 0.3;
        fieldColor.b += sin(distance * 0.08 + u_time * 0.8) * 0.1;
        
        totalField += fieldColor * fieldIntensity;
    }
    
    return totalField;
}

// Volumetric rendering along ray
vec3 volumetricCrystalField(vec3 rayStart, vec3 rayEnd, int samples) {
    vec3 rayDir = rayEnd - rayStart;
    float rayLength = length(rayDir);
    rayDir = normalize(rayDir);
    
    if(rayLength < 0.1) return vec3(0.0);
    
    float stepSize = rayLength / float(samples);
    vec3 step = rayDir * stepSize;
    vec3 currentPos = rayStart + step * 0.5; // Start at half step
    
    vec3 accumulated = vec3(0.0);
    float transmittance = 1.0;
    
    for(int i = 0; i < samples; i++) {
        vec3 fieldContribution = calculateCrystalField(currentPos);
        
        // Simple volumetric integration
        float density = length(fieldContribution) * 0.1;
        float sampleTransmittance = exp(-density * stepSize);
        
        accumulated += fieldContribution * transmittance * stepSize * 10.0;
        transmittance *= sampleTransmittance;
        
        currentPos += step;
        
        // Early exit if transmittance is very low
        if(transmittance < 0.01) break;
    }
    
    return accumulated;
}

void main() {
    // Sample scene data
    vec3 sceneColor = texture(u_sceneColor, TexCoords).rgb;
    float sceneDepth = texture(gDepth, TexCoords).r;
    vec4 material = texture(gMaterial, TexCoords);
    
    // Reconstruct world position
    vec3 worldPos = worldPosFromDepth(TexCoords, sceneDepth);
    
    // Ray from camera to fragment
    vec3 rayStart = u_cameraPos;
    vec3 rayEnd = worldPos;
    float rayLength = length(rayEnd - rayStart);
    
    // Limit ray length for performance
    if(rayLength > 200.0) {
        rayEnd = rayStart + normalize(rayEnd - rayStart) * 200.0;
    }
    
    // Calculate volumetric crystal field effect
    int samples = 16; // Reduced samples for performance
    vec3 fieldEffect = volumetricCrystalField(rayStart, rayEnd, samples);
    
    // Enhance field effect near crystal materials
    uint materialID = uint(round(material.a * 255.0));
    if(materialID == 6u) { // Crystal material
        fieldEffect *= 2.0; // Enhance field around crystals
        
        // Add surface field interaction
        vec3 surfaceField = calculateCrystalField(worldPos);
        fieldEffect += surfaceField * 0.5;
    }
    
    // Apply field effect to scene
    vec3 finalColor = sceneColor + fieldEffect * 0.3;
    
    // Add subtle field glow overlay
    float fieldIntensity = length(fieldEffect);
    if(fieldIntensity > 0.01) {
        // Screen space field enhancement
        vec2 screenOffset = (TexCoords - 0.5) * fieldIntensity * 0.02;
        vec3 glowColor = texture(u_sceneColor, TexCoords + screenOffset).rgb;
        finalColor = mix(finalColor, glowColor, fieldIntensity * 0.1);
    }
    
    FragColor = vec4(finalColor, 1.0);
}
