#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

out vec3 FragPos;
out vec3 Normal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix; // OPTIMIZATION: Pass from CPU

void main()
{
    // Transform position into world space
    FragPos = vec3(model * vec4(aPos, 1.0));

    // OPTIMIZATION: Use pre-calculated normal matrix, instead of slow inverse
    Normal = normalMatrix * aNormal;

    gl_Position = projection * view * vec4(FragPos, 1.0);
}