#version 450 core
layout (location = 0) in vec3 aPos;

// Per-instance data
layout (location = 1) in mat4 aInstanceMatrix; // Model matrix for each chunk cube
layout (location = 2) in vec4 aInstanceColor;  // Color (and alpha) for each chunk cube

out VS_OUT {
    vec4 color;
    vec3 worldPos;
} vs_out;

uniform mat4 projection;
uniform mat4 view;

void main()
{
    vs_out.worldPos = vec3(aInstanceMatrix * vec4(aPos, 1.0));
    vs_out.color = aInstanceColor;
    gl_Position = projection * view * aInstanceMatrix * vec4(aPos, 1.0);
}