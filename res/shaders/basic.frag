#version 450 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;

uniform vec3 lightPos; 
uniform vec3 viewPos;
uniform vec3 lightColor;
uniform vec3 objectColor;

void main()
{
    // Ambient
    float ambientStrength = 0.2;
    vec3 ambient = ambientStrength * lightColor;
      
    // Diffuse 
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor;
    
    // --- SUGGESTION: ADD SPECULAR HIGHLIGHT ---
    float specularStrength = 0.5; // Adjust strength of the highlight
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 halfwayDir = normalize(lightDir + viewDir); // Blinn-Phong is cheaper than Phong
    float spec = pow(max(dot(norm, halfwayDir), 0.0), 32.0); // 32 is the shininess coefficient
    vec3 specular = specularStrength * spec * lightColor;
    // --- END SUGGESTION ---
    
    // Combine results
    vec3 result = (ambient + diffuse + specular) * objectColor;
    FragColor = vec4(result, 1.0);
}