#version 410
out vec4 FragColor;

in vec4 v_ClipSpace;
in vec3 v_ToCameraVector;
in vec3 v_WorldPos;

uniform sampler2D u_ReflectionTexture;
uniform sampler2D u_RefractionTexture;
uniform sampler2D u_DudvMap;
uniform sampler2D u_NormalMap;

uniform vec3 u_LightDirection;
uniform float u_Time;

const float WAVE_STRENGTH = 0.02;

void main()
{
    vec2 ndc = (v_ClipSpace.xy / v_ClipSpace.w) * 0.5 + 0.5;
    
    vec2 texCoords = v_WorldPos.xz * 0.1; // Scale texture coords by world position
    
    vec2 distortion1 = (texture(u_DudvMap, vec2(texCoords.x + u_Time * 0.01, texCoords.y)).rg * 2.0 - 1.0);
    vec2 distortion2 = (texture(u_DudvMap, vec2(texCoords.x - u_Time * 0.013, texCoords.y + u_Time * 0.008)).rg * 2.0 - 1.0);
    vec2 totalDistortion = (distortion1 + distortion2) * WAVE_STRENGTH;

    vec2 refractTexCoords = ndc + totalDistortion;
    vec2 reflectTexCoords = vec2(ndc.x, 1.0 - ndc.y) + totalDistortion;

    refractTexCoords = clamp(refractTexCoords, 0.001, 0.999);
    reflectTexCoords = clamp(reflectTexCoords, 0.001, 0.999);
    
    vec4 reflectColor = texture(u_ReflectionTexture, reflectTexCoords);
    vec4 refractColor = texture(u_RefractionTexture, refractTexCoords);

    vec3 normalMapValue = texture(u_NormalMap, vec2(texCoords.x + u_Time * 0.02, texCoords.y + u_Time * 0.025)).rgb * 2.0 - 1.0;
    vec3 surfaceNormal = normalize(normalMapValue);

    vec3 viewVector = normalize(v_ToCameraVector);
    float fresnelFactor = dot(viewVector, surfaceNormal);
    fresnelFactor = pow(1.0 - fresnelFactor, 3.0);
    fresnelFactor = clamp(fresnelFactor, 0.1, 0.9); // Clamp to avoid full reflection/refraction

    vec3 reflectDir = reflect(-u_LightDirection, surfaceNormal);
    float specular = max(dot(reflectDir, viewVector), 0.0);
    specular = pow(specular, 128.0);
    vec3 specularColor = vec3(1.0) * specular * 0.8;

    vec4 finalColor = mix(refractColor, reflectColor, fresnelFactor);
    finalColor = mix(finalColor, vec4(0.0, 0.1, 0.15, 1.0), 0.2); // Deep water tint
    finalColor += vec4(specularColor, 0.0);

    FragColor = finalColor;
}