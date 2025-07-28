#version 410 core
layout (location = 0) in vec3 a_Position;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

void main()
{
    // Standard transform, then push to the far plane by setting z = w.
    // This ensures the celestial object is always drawn behind everything else.
    gl_Position = u_projection * u_view * u_model * vec4(a_Position, 1.0);
    gl_Position.z = gl_Position.w; 
}