#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;
layout (location = 3) in mat4 aInstanceMatrix; // This is the per-instance model matrix

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;

// Uniforms for camera matrices
uniform mat4 view;
uniform mat4 projection;

void main()
{
    // Use the per-instance matrix for position and normal calculations
    FragPos = vec3(aInstanceMatrix * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(aInstanceMatrix))) * aNormal;
    TexCoords = aTexCoords;

    gl_Position = projection * view * vec4(FragPos, 1.0);
}