#version 410

// Vertex attributes from C++
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in mat4 aInstanceMatrix;

// Outputs to Fragment Shader
out vec3 FragPos;
out vec3 Normal;
out vec4 FragPosLightSpace;

// Uniforms
uniform mat4 projection;
uniform mat4 view;
uniform mat4 lightSpaceMatrix;

void main()
{
    // Transform position and normal to world space
    FragPos = vec3(aInstanceMatrix * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(aInstanceMatrix))) * aNormal;
    
    // Transform world position to light space for shadow mapping
    FragPosLightSpace = lightSpaceMatrix * vec4(FragPos, 1.0);
    
    // Final screen position
    gl_Position = projection * view * vec4(FragPos, 1.0);
}