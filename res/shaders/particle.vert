#version 410
layout (location = 0) in vec3 aPos;

uniform mat4 projection;
uniform mat4 view;

void main()
{
    gl_Position = projection * view * vec4(aPos, 1.0);
    // Make particles smaller the further away they are
    gl_PointSize = 50.0 / gl_Position.w;
}