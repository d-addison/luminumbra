#version 410
layout (location = 0) in vec3 aPos;
// The water mesh has a color attribute at location 2
layout (location = 2) in vec4 aColor;

out vec4
    VertexColor;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    VertexColor = aColor;
    gl_Position = projection * view * model * vec4(aPos, 1.0);
}