#version 410 core
out vec4 FragColor;

in vec3 v_TexCoords;

uniform samplerCube u_skybox;

void main()
{    
    FragColor = texture(u_skybox, v_TexCoords);
}