// res/shaders/basic.vert
#version 330 core

// Input vertex attribute. The '0' corresponds to the location we'll set in C++.
layout (location = 0) in vec3 aPos;

void main()
{
    // gl_Position is a special output variable that determines the final position.
    gl_Position = vec4(aPos, 1.0);
}