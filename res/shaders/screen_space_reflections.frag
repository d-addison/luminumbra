#version 450 core

out vec4 FragColor;

in vec2 TexCoords;

// G-Buffer inputs
uniform sampler2D gPosition;     // View space position
uniform sampler2D gNormal;       // View space normal  
uniform sampler2D gAlbedo;
uniform sampler2D gMaterial;     // R=metallic, G=roughness, B=AO
uniform sampler2D gDepth;

// Scene color for reflections
uniform sampler2D u_sceneColor;

// Camera matrices
uniform mat4 u_view;
uniform mat4 u_projection; 
uniform mat4 u_inverseView;
uniform mat4 u_inverseProjection;
uniform vec3 u_cameraPos;

// SSR settings
uniform float u_ssrStrength = 1.0;
uniform int u_maxSteps = 64;
uniform int u_binarySteps = 16;
uniform float u_stepSize = 1.2;
uniform float u_maxDistance = 100.0;
uniform float u_thickness = 0.5;
uniform float u_roughnessFade = 0.2;

// Screen space parameters
uniform vec2 u_screenSize;

// Noise for dithering
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

// Convert view space to screen space
vec2 viewToScreen(vec3 viewPos) {
    vec4 clipPos = u_projection * vec4(viewPos, 1.0);
    clipPos.xyz /= clipPos.w;
    return clipPos.xy * 0.5 + 0.5;
}

// Reconstruct view position from depth
vec3 reconstructViewPosition(vec2 uv) {
    float depth = texture(gDepth, uv).r;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = u_inverseProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// High quality SSR with hierarchical tracing
vec4 traceScreenSpaceReflection(vec3 viewPos, vec3 viewNormal, float roughness) {
    // Calculate reflection ray
    vec3 viewDir = normalize(viewPos);
    vec3 reflectDir = normalize(reflect(viewDir, viewNormal));
    
    // Early exit for rays pointing toward camera
    if (reflectDir.z > 0.0) {
        return vec4(0.0);
    }
    
    // Ray parameters
    vec3 rayStart = viewPos;
    vec3 rayDir = reflectDir;
    
    // Adaptive step size based on roughness
    float adaptiveStepSize = u_stepSize * (1.0 + roughness * 2.0);
    
    // Screen space ray marching
    vec2 startScreen = viewToScreen(rayStart);
    vec2 endScreen = viewToScreen(rayStart + rayDir * u_maxDistance);
    
    // Ensure we're tracing in screen space
    vec2 deltaScreen = endScreen - startScreen;
    float maxScreenDist = max(abs(deltaScreen.x), abs(deltaScreen.y));
    
    if (maxScreenDist < 0.001) {
        return vec4(0.0); // Ray too short in screen space
    }
    
    // Adjust step count based on screen distance
    int steps = min(u_maxSteps, int(maxScreenDist * u_screenSize.x * 0.5));
    
    vec2 screenStep = deltaScreen / float(steps);
    vec3 viewStep = rayDir * (u_maxDistance / float(steps));
    
    // Add jittering to reduce banding
    float jitter = hash(startScreen + viewPos.xy) * 0.5 + 0.5;
    
    vec2 currentScreen = startScreen + screenStep * jitter;
    vec3 currentView = rayStart + viewStep * jitter;
    
    // Trace ray
    for (int i = 0; i < steps; i++) {
        // Boundary checks
        if (any(lessThan(currentScreen, vec2(0.0))) || 
            any(greaterThan(currentScreen, vec2(1.0)))) {
            break;
        }
        
        // Sample scene depth
        float sceneDepth = texture(gDepth, currentScreen).r;
        vec3 sceneViewPos = reconstructViewPosition(currentScreen);
        
        // Check intersection
        float rayDepth = currentView.z;
        float depthDiff = rayDepth - sceneViewPos.z;
        
        if (depthDiff > 0.0 && depthDiff < u_thickness) {
            // Found intersection - refine with binary search
            vec2 refinedScreen = currentScreen;
            
            // Binary search for more accurate intersection
            vec2 searchStart = currentScreen - screenStep;
            vec2 searchEnd = currentScreen;
            
            for (int j = 0; j < u_binarySteps; j++) {
                vec2 searchMid = (searchStart + searchEnd) * 0.5;
                vec3 searchViewPos = reconstructViewPosition(searchMid);
                vec3 searchRayPos = rayStart + rayDir * length(searchViewPos - rayStart);
                
                if (searchRayPos.z - searchViewPos.z > 0.0) {
                    searchEnd = searchMid;
                } else {
                    searchStart = searchMid;
                }
            }
            
            refinedScreen = (searchStart + searchEnd) * 0.5;
            
            // Sample reflection color
            vec3 reflectionColor = texture(u_sceneColor, refinedScreen).rgb;
            
            // Calculate fade factors
            float edgeFade = 1.0;
            
            // Screen edge fade
            vec2 screenFade = vec2(
                smoothstep(0.0, 0.1, refinedScreen.x) * smoothstep(1.0, 0.9, refinedScreen.x),
                smoothstep(0.0, 0.1, refinedScreen.y) * smoothstep(1.0, 0.9, refinedScreen.y)
            );
            edgeFade *= screenFade.x * screenFade.y;
            
            // Distance fade
            float distance = length(currentView - rayStart);
            float distanceFade = 1.0 - smoothstep(u_maxDistance * 0.7, u_maxDistance, distance);
            edgeFade *= distanceFade;
            
            // Roughness fade
            float roughnessFade = 1.0 - smoothstep(0.0, u_roughnessFade, roughness);
            edgeFade *= roughnessFade;
            
            // Fresnel approximation
            float fresnel = pow(1.0 - max(0.0, dot(-viewDir, viewNormal)), 5.0);
            edgeFade *= fresnel;
            
            return vec4(reflectionColor, edgeFade * u_ssrStrength);
        }
        
        // Advance ray
        currentScreen += screenStep * adaptiveStepSize;
        currentView += viewStep * adaptiveStepSize;
    }
    
    return vec4(0.0); // No intersection found
}

void main() {
    // Sample G-Buffer
    vec3 viewPos = texture(gPosition, TexCoords).rgb;
    vec3 viewNormal = normalize(texture(gNormal, TexCoords).rgb);
    vec3 albedo = texture(gAlbedo, TexCoords).rgb;
    vec4 material = texture(gMaterial, TexCoords);
    
    float metallic = material.r;
    float roughness = clamp(material.g, 0.05, 1.0);
    
    // Early exit for non-reflective surfaces
    if (metallic < 0.1 && roughness > 0.8) {
        FragColor = vec4(0.0);
        return;
    }
    
    // Only compute reflections for valid surface pixels
    if (length(viewNormal) < 0.1) {
        FragColor = vec4(0.0);
        return;
    }
    
    // Trace reflection
    vec4 reflection = traceScreenSpaceReflection(viewPos, viewNormal, roughness);
    
    // Modulate by material properties
    float reflectivity = mix(0.04, 1.0, metallic); // Dielectric vs metallic
    reflectivity *= (1.0 - roughness * 0.7); // Roughness reduces reflections
    
    reflection.a *= reflectivity;
    
    // For metallic surfaces, tint reflection with albedo
    if (metallic > 0.1) {
        reflection.rgb *= albedo;
    }
    
    FragColor = reflection;
}