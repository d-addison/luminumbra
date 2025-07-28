#version 410
layout (location = 0) in vec3 a_Position;

out vec4 v_ClipSpace;
out vec3 v_ToCameraVector;
out vec3 v_WorldPos;

uniform mat4 u_Projection;
uniform mat4 u_View;
uniform mat4 u_Model;
uniform vec3 u_CameraPosition;

void main()
{
    vec4 worldPos4 = u_Model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos4.xyz;

    v_ClipSpace = u_Projection * u_View * worldPos4;
    gl_Position = v_ClipSpace;

    v_ToCameraVector = u_CameraPosition - v_WorldPos;
}