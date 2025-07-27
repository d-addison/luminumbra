#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec3 Color;
in vec4 FragPosLightSpace;

uniform vec3 viewPos;
uniform vec3 fogColor;
uniform vec3 sunDirection;
uniform sampler2D shadowMap;

float ShadowCalculation(vec4 fragPosLightSpace)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0)
        return 0.0;

    float shadow = 0.0;
    float bias = 0.005;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for(int x = -1; x <= 1; ++x)
    {
        for(int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r; 
            shadow += projCoords.z - bias > pcfDepth ? 1.0 : 0.0;        
        }
    }
    shadow /= 9.0;
    // return shadow;
    return 0;
}

void main()
{
    // Dynamic lighting based on sun position
    vec3 lightDir = normalize(-sunDirection);
    
    // Light color changes with time of day
    vec3 dayLightColor = vec3(1.0, 0.95, 0.85); // Warm sunlight
    vec3 sunriseSunsetColor = vec3(1.0, 0.6, 0.3); // Orange/pink
    vec3 nightLightColor = vec3(0.2, 0.3, 0.5); // Moonlight blue
    
    vec3 lightColor;
    float sunHeight = sunDirection.y;
    
    if (sunHeight > 0.0) {
        // Daytime - blend between sunrise/sunset and noon colors
        float t = smoothstep(0.0, 0.5, sunHeight);
        lightColor = mix(sunriseSunsetColor, dayLightColor, t);
    } else {
        // Night time
        lightColor = nightLightColor;
    }
    
    // Ambient lighting varies with time of day
    float ambientStrength = mix(0.15, 0.4, clamp(sunHeight + 1.0, 0.0, 1.0));
    vec3 ambient = ambientStrength * lightColor * Color;
    
    // Diffuse lighting
    vec3 norm = normalize(Normal);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor * Color;

    // Specular lighting
    float specularStrength = 0.5;
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
    vec3 specular = specularStrength * spec * lightColor;
    
    // Calculate shadow
    // float shadow = ShadowCalculation(FragPosLightSpace);
    vec3 lighting = (ambient + (1.0 - 0) * (diffuse + specular));
    
    // Simple fog effect based on distance
    float distance = length(viewPos - FragPos);
    float fogStart = 500.0;
    float fogEnd = 1000.0;
    float fogFactor = clamp((fogEnd - distance) / (fogEnd - fogStart), 0.0, 1.0);
    
    // Apply fog
    vec3 result = mix(fogColor, lighting, fogFactor);
    
    FragColor = vec4(result, 1.0);
}
