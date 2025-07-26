#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

out vec3 FragPos;
out vec3 Normal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    // Transform position and normal into world space
    FragPos = vec3(model * vec4(aPos, 1.0));
    // Use transpose(inverse(model)) to correctly transform normals, especially with non-uniform scaling
    Normal = mat3(transpose(inverse(model))) * aNormal;
    
    gl_Position = projection * view * vec4(FragPos, 1.0);
}