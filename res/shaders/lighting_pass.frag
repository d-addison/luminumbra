#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;

uniform vec3 viewPos;

void main()
{
    // retrieve data from G-buffer
    vec3 FragPos = texture(gPosition, TexCoords).rgb;
    vec3 Normal = texture(gNormal, TexCoords).rgb;
    vec3 Albedo = texture(gAlbedoSpec, TexCoords).rgb;
    float Specular = texture(gAlbedoSpec, TexCoords).a;

    // simple directional light
    vec3 lightDir = normalize(vec3(0.5, 0.5, -0.5));
    vec3 lightColor = vec3(1.0);
    
    // ambient
    vec3 ambient = 0.1 * Albedo;
    // diffuse
    vec3 viewDir = normalize(viewPos - FragPos);
    float diff = max(dot(Normal, lightDir), 0.0);
    vec3 diffuse = diff * Albedo * lightColor;
    // specular
    vec3 reflectDir = reflect(-lightDir, Normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    vec3 specular = Specular * spec * lightColor;

    FragColor = vec4(ambient + diffuse + specular, 1.0);
}
