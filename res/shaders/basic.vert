// res/shaders/basic.vert
#version 330 core
layout (location = 0) in vec3 aPos;

// Uniforms are global variables for a shader program
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    // Transform vertex position into clip space
    gl_Position = projection * view * model * vec4(aPos, 1.0);
}