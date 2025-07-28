#version 410
out vec4 FragColor;

in vec3 Normal;

void main()
{
    // Visualize normals by mapping them to RGB colors
    vec3 norm = normalize(Normal);
    vec3 color = norm * 0.5 + 0.5; // Transform normals from [-1,1] to [0,1] range
    FragColor = vec4(color, 1.0);
}
