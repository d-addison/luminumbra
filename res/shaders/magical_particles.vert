#version 450 core

// Per-vertex attributes
layout (location = 0) in vec3 aPosition;    // World position
layout (location = 1) in vec3 aVelocity;    // Velocity vector
layout (location = 2) in float aLifetime;   // Current lifetime
layout (location = 3) in float aMaxLifetime; // Maximum lifetime
layout (location = 4) in float aSize;       // Particle size
layout (location = 5) in vec4 aColor;       // Particle color
layout (location = 6) in float aType;       // Particle type (0=sparkle, 1=ember, 2=magic, 3=crystal)

// Uniforms
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_cameraPos;
uniform float u_time;
uniform vec3 u_windDirection = vec3(1.0, 0.0, 0.0);
uniform float u_windStrength = 0.5;

// Output to geometry shader
out VS_OUT {
    vec3 worldPos;
    vec4 color;
    float size;
    float lifetime;
    float maxLifetime;
    float type;
    float distanceToCamera;
} vs_out;

// Noise function for organic movement
float hash(float p) {
    return fract(sin(p * 127.1) * 43758.5453123);
}

float noise(float p) {
    float i = floor(p);
    float f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), f);
}

void main() {
    // Calculate age factor (0 = just born, 1 = about to die)
    float ageFactor = aLifetime / aMaxLifetime;
    
    // Apply wind and turbulence to position
    vec3 worldPos = aPosition;
    
    // Wind effect
    worldPos += u_windDirection * u_windStrength * aLifetime * 0.1;
    
    // Add organic turbulence based on particle type and position
    float turbulenceScale = 0.01;
    float turbulenceStrength = 0.5;
    
    if (aType == 0.0) { // Sparkle particles - gentle floating
        float noiseTime = u_time * 0.5 + aPosition.x * 0.1;
        worldPos.x += sin(noiseTime) * 0.3;
        worldPos.y += cos(noiseTime * 0.7) * 0.2;
        worldPos.z += sin(noiseTime * 0.3) * 0.25;
    }
    else if (aType == 1.0) { // Ember particles - rising with heat distortion
        float heat = 1.0 - ageFactor;
        worldPos.y += heat * aLifetime * 0.5; // Rise with heat
        
        float flicker = noise(u_time * 3.0 + aPosition.x * 10.0) * 0.2;
        worldPos.x += flicker * heat;
        worldPos.z += flicker * heat * 0.5;
    }
    else if (aType == 2.0) { // Magic particles - spiral movement
        float spiral = u_time * 2.0 + aPosition.y * 0.1;
        float radius = 0.5 * (1.0 - ageFactor);
        worldPos.x += cos(spiral) * radius;
        worldPos.z += sin(spiral) * radius;
        worldPos.y += sin(spiral * 0.3) * 0.3;
    }
    else if (aType == 3.0) { // Crystal particles - geometric patterns
        float geometric = u_time + aPosition.x * 0.2 + aPosition.z * 0.15;
        worldPos.y += sin(geometric * 4.0) * 0.1;
        worldPos.x += cos(geometric * 6.0) * 0.15;
        worldPos.z += sin(geometric * 5.0) * 0.12;
    }
    
    // Calculate distance to camera for LOD
    float distanceToCamera = length(worldPos - u_cameraPos);
    
    // Size attenuation based on distance and age
    float sizeScale = aSize;
    
    // Particles grow slightly when young, shrink when old
    if (ageFactor < 0.3) {
        sizeScale *= (0.5 + ageFactor * 1.5); // Grow from 50% to 95%
    } else {
        sizeScale *= (1.2 - ageFactor * 0.7); // Shrink from 95% to 50%
    }
    
    // Distance-based size scaling for performance
    sizeScale *= clamp(50.0 / distanceToCamera, 0.3, 2.0);
    
    // Color evolution over lifetime
    vec4 evolvedColor = aColor;
    
    if (aType == 0.0) { // Sparkle - fade to white
        evolvedColor.rgb = mix(aColor.rgb, vec3(1.0, 1.0, 1.0), ageFactor * 0.5);
        evolvedColor.a *= (1.0 - pow(ageFactor, 2.0)); // Quadratic fade
    }
    else if (aType == 1.0) { // Ember - cool down
        evolvedColor.rgb = mix(vec3(1.0, 0.3, 0.1), vec3(0.8, 0.1, 0.05), ageFactor);
        evolvedColor.a *= (1.0 - ageFactor);
    }
    else if (aType == 2.0) { // Magic - cycle through colors
        float colorCycle = sin(u_time * 2.0 + aPosition.y * 0.1) * 0.5 + 0.5;
        evolvedColor.rgb = mix(aColor.rgb, vec3(0.8, 0.2, 1.0), colorCycle * 0.3);
        evolvedColor.a *= pow(1.0 - ageFactor, 1.5);
    }
    else if (aType == 3.0) { // Crystal - prismatic effects
        float prism = sin(u_time + ageFactor * 6.28) * 0.5 + 0.5;
        evolvedColor.rgb = aColor.rgb + vec3(0.2, 0.1, 0.3) * prism;
        evolvedColor.a *= (1.0 - pow(ageFactor, 0.8));
    }
    
    // Pass data to geometry shader
    vs_out.worldPos = worldPos;
    vs_out.color = evolvedColor;
    vs_out.size = sizeScale;
    vs_out.lifetime = aLifetime;
    vs_out.maxLifetime = aMaxLifetime;
    vs_out.type = aType;
    vs_out.distanceToCamera = distanceToCamera;
    
    // Transform to clip space
    gl_Position = u_projection * u_view * vec4(worldPos, 1.0);
}