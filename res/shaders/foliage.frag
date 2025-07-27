#version 410
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;

uniform vec3 objectColor;
uniform vec3 sunDirection;
uniform vec3 viewPos;
uniform vec3 fogColor;

void main()
{
    // Ambient lighting
    float ambientStrength = 0.4;
    vec3 ambient = ambientStrength * objectColor;

    // Diffuse lighting
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(-sunDirection);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * objectColor;

    vec3 result = ambient + diffuse;

    // Fog
    float dist = length(viewPos - FragPos);
    float fogStart = 80.0;
    float fogEnd = 160.0;
    float fogFactor = clamp((dist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    
    FragColor = vec4(mix(result, fogColor, fogFactor), 1.0);
}