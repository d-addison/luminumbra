#version 450 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform mat4 u_model;
uniform mat3 u_normal;
uniform mat4 u_view;
uniform mat4 u_projection;
out vec3 worldPosition;
out vec3 worldNormal;
out vec3 viewPosition;
out vec2 rawUv;
void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    worldPosition = world.xyz;
    worldNormal = normalize(u_normal * a_normal);
    vec4 view = u_view * world;
    viewPosition = view.xyz;
    rawUv = a_uv;
    gl_Position = u_projection * view;
}
