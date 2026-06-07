#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

// G-Buffer inputs
uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gDepth;

// Shadow maps
uniform sampler2DArray u_shadowCascades;
uniform mat4 u_lightSpaceMatrices[4];
uniform vec4 u_cascadeSplits;

// Scene uniforms
uniform mat4 u_inverseView;
uniform mat4 u_inverseProjection;
uniform vec3 u_viewPos;
uniform vec3 u_sunDirection;
uniform vec3 u_sunColor;
uniform float u_sunIntensity;
uniform float u_time;

// Volumetric settings
uniform float u_scatteringStrength = 0.1;
uniform float u_extinctionStrength = 0.05;
uniform int u_numSamples = 32;
uniform float u_maxDistance = 200.0;
uniform float u_fogDensity = 0.3;

// Noise for atmospheric variation
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

// Fractal noise for realistic fog
float fbmNoise(vec3 p) {
    float value = 0.0;
    float amplitude = 0.5;
    
    for(int i = 0; i < 4; i++) {
        value += amplitude * noise3D(p);
        p *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

// Calculate shadow at a world position
float calculateVolumetricShadow(vec3 worldPos) {
    // Convert to view space for cascade selection
    vec4 viewPos = u_inverseView * vec4(worldPos, 1.0);
    float viewDepth = abs(viewPos.z);
    
    // Determine shadow cascade
    int cascadeIndex = 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.x) ? 1 : 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.y) ? 1 : 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.z) ? 1 : 0;
    
    // Project to light space
    vec4 lightSpacePos = u_lightSpaceMatrices[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5;
    
    if(projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || 
       projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 1.0; // Outside shadow map
    }
    
    // Simple shadow test for volumetrics (no PCF for performance)
    float shadowDepth = texture(u_shadowCascades, vec3(projCoords.xy, cascadeIndex)).r;
    float bias = 0.005;
    
    return (projCoords.z - bias > shadowDepth) ? 0.2 : 1.0; // Some ambient even in shadow
}

// Henyey-Greenstein phase function for realistic scattering
float phaseFunction(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265359 * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

vec3 worldPositionFromDepth(vec2 uv, float depth) {
    // Reconstruct world position from depth
    float z = depth * 2.0 - 1.0;
    vec4 clipSpacePos = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 viewSpacePos = u_inverseProjection * clipSpacePos;
    viewSpacePos /= viewSpacePos.w;
    vec4 worldSpacePos = u_inverseView * viewSpacePos;
    return worldSpacePos.xyz;
}

void main() {
    // Get scene depth
    float sceneDepth = texture(gDepth, TexCoords).r;
    vec3 sceneWorldPos = worldPositionFromDepth(TexCoords, sceneDepth);
    
    // Ray from camera to scene
    vec3 rayStart = u_viewPos;
    vec3 rayEnd = sceneWorldPos;
    vec3 rayDir = rayEnd - rayStart;
    float rayLength = length(rayDir);
    rayDir = normalize(rayDir);
    
    // Limit ray length for performance
    rayLength = min(rayLength, u_maxDistance);
    rayEnd = rayStart + rayDir * rayLength;
    
    // Volumetric raymarching
    float stepSize = rayLength / float(u_numSamples);
    vec3 step = rayDir * stepSize;
    vec3 currentPos = rayStart + step * 0.5; // Start at half step to avoid self-intersection
    
    vec3 scattering = vec3(0.0);
    float totalExtinction = 0.0;
    
    // Phase function for forward scattering (god rays)
    float cosTheta = dot(rayDir, -u_sunDirection);
    float phase = phaseFunction(cosTheta, 0.76); // Forward scattering
    
    for(int i = 0; i < u_numSamples && i < int(rayLength / stepSize); i++) {
        // Dynamic fog density based on height and noise
        float heightFactor = exp(-max(0.0, currentPos.y - 0.0) * 0.01); // Sea level fog
        float noiseFactor = fbmNoise(currentPos * 0.01 + vec3(u_time * 0.02, 0.0, u_time * 0.01));
        noiseFactor = noiseFactor * 0.5 + 0.5; // Normalize to [0,1]
        
        float density = u_fogDensity * heightFactor * noiseFactor;
        
        // Calculate shadow/lighting at this point
        float shadow = calculateVolumetricShadow(currentPos);
        
        // Sun contribution
        vec3 sunContribution = u_sunColor * u_sunIntensity * shadow * phase;
        
        // Add some ambient scattering
        vec3 ambientContribution = vec3(0.1, 0.15, 0.3) * 0.3;
        
        // Accumulate scattering
        vec3 lightContribution = (sunContribution + ambientContribution) * density;
        
        // Apply extinction
        float extinction = density * u_extinctionStrength;
        float transmittance = exp(-totalExtinction);
        
        scattering += lightContribution * transmittance * u_scatteringStrength;
        totalExtinction += extinction;
        
        currentPos += step;
    }
    
    // Final transmittance
    float finalTransmittance = exp(-totalExtinction);
    
    // God rays enhancement near sun direction
    float godRayFactor = pow(max(0.0, cosTheta), 4.0) * u_sunIntensity;
    scattering += u_sunColor * godRayFactor * 0.1;
    
    // Output accumulated scattering and transmittance
    // RGB = scattering, A = transmittance (for blending with scene)
    FragColor = vec4(scattering, finalTransmittance);
}