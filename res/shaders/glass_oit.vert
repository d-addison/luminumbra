#version 450 core
// glass panes into the WBOIT accumulation targets.
layout (location = 0) in vec3 aPos;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 vWorldPos;
out vec3 vWorldNormal;
out float vViewDepth;

void main() {
    vec4 world = u_model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    // The pane quad lies in its local XY plane; +Z is its face normal.
    vWorldNormal = normalize(mat3(u_model) * vec3(0.0, 0.0, 1.0));
    vec4 view = u_view * world;
    vViewDepth = -view.z;
    gl_Position = u_projection * view;
}
