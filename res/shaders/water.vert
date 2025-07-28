#version 410 core
layout (location = 0) in vec3 a_Position;

out vec4 v_clipSpace;
out vec3 v_toCameraVector;
out vec3 v_worldPos;

uniform mat4 u_projection;
uniform mat4 u_view;
uniform mat4 u_model;
uniform vec3 u_cameraPosition;

void main()
{
    vec4 worldPos4 = u_model * vec4(a_Position, 1.0);
    v_worldPos = worldPos4.xyz;

    v_clipSpace = u_projection * u_view * worldPos4;
    gl_Position = v_clipSpace;

    v_toCameraVector = u_cameraPosition - v_worldPos;
}