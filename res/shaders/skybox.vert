#version 450 core
layout (location = 0) in vec3 aPos;

out vec3 WorldPos;

uniform mat4 projection;
uniform mat4 view; // This is the rotation-only matrix from the C++ code

void main()
{
    WorldPos = aPos;
    
    // Calculate the clip-space position as usual.
    vec4 pos = projection * view * vec4(aPos, 1.0);

    // The reversed-Z far plane is zero under GL_ZERO_TO_ONE.
    gl_Position = pos;
    gl_Position.z = 0.0;
}
