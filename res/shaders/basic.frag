#version 410 core
out vec4 FragColor;

// From Vertex Shader
in vec3 v_FragPos;
in vec3 v_Normal;
in vec3 v_Color;
in vec4 v_FragPosLightSpace;

// Uniforms
uniform vec3 u_viewPos;
uniform vec3 u_sunDirection;
uniform vec3 u_fogColor;
uniform sampler2D u_shadowMap;

// Improved PCF shadow calculation for smoother shadows
float calculateShadow(vec4 fragPosLightSpace)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0) {
        return 0.0;
    }

    float shadow = 0.0;
    // Use a dynamic bias to prevent shadow acne on surfaces parallel to the light
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
    // 1. Lighting Color Calculation (Day/Night cycle)
    vec3 lightDir = normalize(-u_sunDirection);
    vec3 dayLightColor = vec3(1.0, 0.95, 0.85);
    vec3 sunriseSunsetColor = vec3(1.0, 0.6, 0.3);
    vec3 nightLightColor = vec3(0.2, 0.3, 0.5);
    
    vec3 lightColor;
    float sunHeight = u_sunDirection.y;
    
    if (sunHeight > 0.0) {
        float t = smoothstep(0.0, 0.5, sunHeight);
        lightColor = mix(sunriseSunsetColor, dayLightColor, t);
    } else {
        lightColor = nightLightColor;
    }
    
    // 2. Standard Lighting Model (Ambient, Diffuse, Specular)
    float ambientStrength = mix(0.15, 0.4, clamp(sunHeight + 1.0, 0.0, 1.0));
    vec3 ambient = ambientStrength * lightColor * v_Color;
    
    vec3 norm = normalize(v_Normal);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor * v_Color;

    float specularStrength = 0.5;
    vec3 viewDir = normalize(u_viewPos - v_FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
    vec3 specular = specularStrength * spec * lightColor;
    
    // 3. Shadow Calculation
    float shadow = calculateShadow(v_FragPosLightSpace);
    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular));
    
    // 4. Fog
    float distance = length(u_viewPos - v_FragPos);
    float fogStart = 200.0;
    float fogEnd = 400.0;
    float fogFactor = 1.0 - clamp((fogEnd - distance) / (fogEnd - fogStart), 0.0, 1.0);
    
    vec3 result = mix(lighting, u_fogColor, fogFactor);
    
    FragColor = vec4(result, 1.0);
}