#version 410

// Output color
out vec4 FragColor;

// Inputs from Vertex Shader
in vec3 FragPos;
in vec3 Normal;
in float v_ClipDistance;
in vec4 FragPosLightSpace;

// Uniforms
uniform vec3 objectColor;
uniform vec3 sunDirection;
uniform vec3 viewPos;
uniform vec3 fogColor;
uniform sampler2D shadowMap;

float calculateShadow()
{
    if (v_ClipDistance < 0.0) {
        discard;
    }
    // Perform perspective divide to get normalized device coordinates
    vec3 projCoords = FragPosLightSpace.xyz / FragPosLightSpace.w;
    
    // Transform to [0,1] texture coordinate range
    projCoords = projCoords * 0.5 + 0.5;
    
    // Get depth of the closest shadow-casting object from the light's POV
    float closestDepth = texture(shadowMap, projCoords.xy).r;
    
    // Get current fragment's depth from the light's POV
    float currentDepth = projCoords.z;
    
    // Check if the current fragment is behind the closest object (and thus in shadow)
    // Add a small bias to prevent shadow acne artifacts
    float bias = 0.005;
    float shadow = currentDepth - bias > closestDepth ? 1.0 : 0.0;
    
    // Don't cast shadows on fragments that are outside the light's view
    if(projCoords.z > 1.0) {
        shadow = 0.0;
    }
        
    return shadow;
}

void main()
{
    // 1. Calculate Shadow
    float shadow = calculateShadow();

    // 2. Calculate Lighting
    // Ambient
    float ambientStrength = 0.4;
    vec3 ambient = ambientStrength * objectColor;

    // Diffuse
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(sunDirection.y < 0.0 ? -sunDirection : sunDirection);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * objectColor;
    
    // Combine lighting, applying shadow only to the diffuse component
    vec3 lighting = ambient + (1.0 - shadow) * diffuse;

    // 3. Apply Fog
    float dist = length(viewPos - FragPos);
    float fogStart = 300.0;
    float fogEnd = 500.0;
    float fogFactor = clamp((dist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    
    vec3 finalColor = mix(lighting, fogColor, fogFactor);
    
    FragColor = vec4(finalColor, 1.0);
}