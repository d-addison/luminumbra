#version 410
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in vec3 a_Color;

out vec3 Normal;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

void main()
{
    Normal = mat3(transpose(inverse(u_model))) * a_Normal;
    gl_Position = u_projection * u_view * u_model * vec4(a_Position, 1.0);
}
