#version 410
out vec4 FragColor;

uniform vec3 objectColor;
uniform float brightness;

void main()
{
    // Apply brightness to the color channels and set alpha to 1.0 for opaque objects.
    FragColor = vec4(objectColor * brightness, 1.0);
}