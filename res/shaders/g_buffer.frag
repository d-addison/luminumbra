#version 450 core
layout (location = 0) out vec3 gPosition;
layout (location = 1) out vec3 gNormal;
layout (location = 2) out vec4 gAlbedoSpec;

in vec3 FragPos;
in vec3 Normal;
flat in uint MaterialID;

// uniform sampler2D texture_diffuse1;

void main()
{
    gPosition = FragPos;
    gNormal = normalize(Normal);
    
    vec3 color = vec3(0.95); // Default white
    if (MaterialID == 1) {
        color = vec3(0.2, 0.8, 0.2); // Green for grass
    } else if (MaterialID == 2) {
        color = vec3(0.5, 0.5, 0.5); // Grey for rock
    }

    gAlbedoSpec.rgb = color;
    gAlbedoSpec.a = 1.0; // Specular intensity
}
