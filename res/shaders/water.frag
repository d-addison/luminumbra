#version 410
out vec4 FragColor;

in vec4 v_ClipSpace;
in vec3 v_ToCameraVector;
in vec3 v_WorldPos;

// Samplers
uniform sampler2D u_ReflectionTexture;
uniform sampler2D u_RefractionTexture;
uniform sampler2D u_DudvMap;
uniform sampler2D u_NormalMap;
uniform sampler2D u_RefractionDepthTexture; // NEW: Depth texture

// Uniforms
uniform vec3 u_LightDirection;
uniform float u_Time;
uniform float u_NearPlane; // NEW
uniform float u_FarPlane;  // NEW

// Constants for water appearance
const float WAVE_STRENGTH = 0.02;
const float SHINE_DAMPER = 128.0;
const float SPECULARITY = 0.8;
const vec4 MURKY_COLOR = vec4(0.0, 0.1, 0.15, 1.0); // The deep, opaque color of the water
const float MURKINESS_DENSITY = 30.0; // How quickly the water becomes opaque

// Converts a non-linear depth value from the depth buffer to a linear distance
float LinearizeDepth(float depth)
{
    float z_ndc = depth * 2.0 - 1.0; // Convert [0,1] range to [-1,1] NDC
    return (2.0 * u_NearPlane * u_FarPlane) / (u_FarPlane + u_NearPlane - z_ndc * (u_FarPlane - u_NearPlane));    
}

void main()
{
    // Screen-space texture coordinates
    vec2 ndc = (v_ClipSpace.xy / v_ClipSpace.w) * 0.5 + 0.5;
    
    // World-space texture coordinates for distortion and normals
    vec2 worldTexCoords = v_WorldPos.xz * 0.1; 
    
    // DUDV distortion for water surface movement
    vec2 distortion1 = (texture(u_DudvMap, vec2(worldTexCoords.x + u_Time * 0.01, worldTexCoords.y)).rg * 2.0 - 1.0);
    vec2 distortion2 = (texture(u_DudvMap, vec2(worldTexCoords.x - u_Time * 0.013, worldTexCoords.y + u_Time * 0.008)).rg * 2.0 - 1.0);
    vec2 totalDistortion = (distortion1 + distortion2) * WAVE_STRENGTH;

    // Apply distortion to refraction and reflection coordinates
    vec2 refractTexCoords = clamp(ndc + totalDistortion, 0.001, 0.999);
    vec2 reflectTexCoords = clamp(vec2(ndc.x, 1.0 - ndc.y) + totalDistortion, 0.001, 0.999);
    
    // Get base reflection and refraction colors
    vec4 reflectColor = texture(u_ReflectionTexture, reflectTexCoords);
    vec4 refractColor = texture(u_RefractionTexture, refractTexCoords);

    reflectColor.rgb *= 0.6;

    // Get surface normal from normal map for lighting
    vec3 surfaceNormal = normalize(texture(u_NormalMap, vec2(worldTexCoords.x + u_Time * 0.02, worldTexCoords.y + u_Time * 0.025)).rgb * 2.0 - 1.0);

    // === NEW: Depth-based Murkiness Calculation ===
    float floorDepth = texture(u_RefractionDepthTexture, refractTexCoords).r;
    float waterSurfaceDepth = gl_FragCoord.z;
    
    float linearFloorDepth = LinearizeDepth(floorDepth);
    float linearSurfaceDepth = LinearizeDepth(waterSurfaceDepth);
    
    float waterColumnDepth = linearFloorDepth - linearSurfaceDepth;
    
    // Use the depth to create a fog-like effect for the water
    float murkFactor = clamp(waterColumnDepth / MURKINESS_DENSITY, 0.0, 1.0);
    refractColor = mix(refractColor, MURKY_COLOR, murkFactor);
    // ===============================================

    // Fresnel effect to blend reflection and refraction
    vec3 viewVector = normalize(v_ToCameraVector);
    float fresnelFactor = dot(viewVector, surfaceNormal);
    fresnelFactor = pow(1.0 - fresnelFactor, 3.0);
    fresnelFactor = clamp(fresnelFactor, 0.1, 0.9); 

    // Specular lighting for sun glare
    vec3 reflectDir = reflect(-u_LightDirection, surfaceNormal);
    float specular = max(dot(reflectDir, viewVector), 0.0);
    specular = pow(specular, SHINE_DAMPER);
    vec3 specularColor = vec3(1.0) * specular * SPECULARITY;

    // Combine all components for the final color
    vec4 finalColor = mix(refractColor, reflectColor, fresnelFactor);
    finalColor += vec4(specularColor, 0.0);

    FragColor = finalColor;
}