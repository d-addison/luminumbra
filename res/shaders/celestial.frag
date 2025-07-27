#version 410
out vec4 FragColor;

uniform vec3 objectColor;
uniform float brightness;

void main()
{
    FragColor = vec4(objectColor, brightness);
}