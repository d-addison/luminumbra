#version 410 core
out vec4 FragColor;

in vec3 v_FragPos;
in vec3 v_Normal;
in vec4 v_FragPosLightSpace;

uniform vec3 u_viewPos;
uniform vec3 u_sunDirection;
uniform vec3 u_fogColor;
uniform sampler2D u_shadowMap;

// Consistent PCF shadow calculation
float calculateShadow(vec4 fragPosLightSpace)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0) {
        return 0.0;
    }

    float shadow = 0.0;
    float bias = max(0.05 * (1.0 - dot(normalize(v_Normal), -u_sunDirection)), 0.005);
    vec2 texelSize = 1.0 / textureSize(u_shadowMap, 0);

    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(u_shadowMap, projCoords.xy + vec2(x, y) * texelSize).r; 
            shadow += projCoords.z - bias > pcfDepth ? 1.0 : 0.0;        
        }
    }
    shadow /= 9.0;
    
    return shadow;
}

void main()
{
    // For foliage, we often use alpha testing to cut out shapes.
    // Assuming a texture could be used, but for now just lighting.
    // if (texture(u_albedo, v_TexCoords).a < 0.5) discard;

    float shadow = calculateShadow(v_FragPosLightSpace);

    // Simplified lighting for performance, more ambient.
    float ambientStrength = 0.5;
    vec3 ambient = ambientStrength * vec3(1.0); // Assuming white foliage color

    vec3 norm = normalize(v_Normal);
    vec3 lightDir = normalize(-u_sunDirection);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * vec3(1.0);
    
    vec3 lighting = ambient + (1.0 - shadow) * diffuse;

    float dist = length(u_viewPos - v_FragPos);
    float fogStart = 80.0;
    float fogEnd = 160.0;
    float fogFactor = clamp((dist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    
    vec3 finalColor = mix(lighting, u_fogColor, fogFactor);
    
    FragColor = vec4(finalColor, 1.0);
}