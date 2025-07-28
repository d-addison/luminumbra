#version 410 core
out vec4 FragColor;

in vec4 v_clipSpace;
in vec3 v_toCameraVector;
in vec3 v_worldPos;

uniform sampler2D u_reflectionTexture;
uniform sampler2D u_refractionTexture;
uniform sampler2D u_dudvMap;
uniform sampler2D u_normalMap;

uniform vec3 u_lightDirection;
uniform float u_time;

const float WAVE_STRENGTH = 0.02;

void main()
{
    // Project fragment to screen space for texture lookups
    vec2 ndc = (v_clipSpace.xy / v_clipSpace.w) * 0.5 + 0.5;
    
    // Use world position for tiling DUDV and normal maps to avoid stretching
    vec2 tileCoords = v_worldPos.xz * 0.1;
    
    // Combine two scrolling distortion maps for more complex wave patterns
    vec2 distortion1 = (texture(u_dudvMap, vec2(tileCoords.x + u_time * 0.01, tileCoords.y)).rg * 2.0 - 1.0);
    vec2 distortion2 = (texture(u_dudvMap, vec2(tileCoords.x - u_time * 0.013, tileCoords.y + u_time * 0.008)).rg * 2.0 - 1.0);
    vec2 totalDistortion = (distortion1 + distortion2) * WAVE_STRENGTH;

    vec2 refractTexCoords = ndc + totalDistortion;
    vec2 reflectTexCoords = vec2(ndc.x, 1.0 - ndc.y) + totalDistortion;

    refractTexCoords = clamp(refractTexCoords, 0.001, 0.999);
    reflectTexCoords = clamp(reflectTexCoords, 0.001, 0.999);
    
    vec4 reflectColor = texture(u_reflectionTexture, reflectTexCoords);
    vec4 refractColor = texture(u_refractionTexture, refractTexCoords);

    // Get surface normal from a scrolling normal map
    vec3 surfaceNormal = normalize(texture(u_normalMap, tileCoords + u_time * 0.015).rgb * 2.0 - 1.0);

    // Fresnel effect to mix reflection and refraction
    vec3 viewVector = normalize(v_toCameraVector);
    float fresnelFactor = dot(viewVector, surfaceNormal);
    fresnelFactor = pow(1.0 - fresnelFactor, 3.0);
    fresnelFactor = clamp(fresnelFactor, 0.1, 0.9);

    // Specular highlights
    vec3 reflectDir = reflect(-u_lightDirection, surfaceNormal);
    float specular = max(dot(reflectDir, viewVector), 0.0);
    specular = pow(specular, 128.0);
    vec3 specularColor = vec3(1.0) * specular * 0.8;

    // Combine all components
    vec4 mixedColor = mix(refractColor, reflectColor, fresnelFactor);
    vec4 finalColor = mix(mixedColor, vec4(0.0, 0.1, 0.15, 1.0), 0.2); // Deep water tint
    finalColor += vec4(specularColor, 0.0);

    FragColor = finalColor;
}