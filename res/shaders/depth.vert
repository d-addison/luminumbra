#version 410 core
layout (location = 0) in vec3 a_Position;

uniform mat4 u_model;
uniform mat4 u_lightSpaceMatrix;

void main()
{
    gl_Position = u_lightSpaceMatrix * u_model * vec4(a_Position, 1.0);
}