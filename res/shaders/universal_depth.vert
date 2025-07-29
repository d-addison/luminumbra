#version 410
layout (location = 0) in vec3 a_Pos;
layout (location = 2) in mat4 a_InstanceMatrix;

uniform mat4 u_Model;
uniform mat4 u_LightSpaceMatrix;
uniform bool u_IsInstanced; // A uniform to control which matrix to use

void main()
{
    mat4 modelMatrix = u_IsInstanced ? a_InstanceMatrix : u_Model;
    gl_Position = u_LightSpaceMatrix * modelMatrix * vec4(a_Pos, 1.0);
}