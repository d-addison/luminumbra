// res/shaders/basic.frag
#version 330 core

// The single output variable for this shader.
out vec4 FragColor;

void main()
{
    // Set the color to a warm gold. (R=1.0, G=0.84, B=0.0)
    FragColor = vec4(1.0f, 0.84f, 0.0f, 1.0f);
}