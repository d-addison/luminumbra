#version 410 core
out vec4 FragColor;

uniform vec3 u_objectColor;
uniform float u_brightness;

void main()
{
    FragColor = vec4(u_objectColor, u_brightness);
}