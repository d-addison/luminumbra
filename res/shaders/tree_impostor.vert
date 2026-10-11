#version 450 core
//  far-field tree impostor. One camera-facing billboard quad per distant tree, drawn instanced.
// gl_VertexID gives the quad corner (triangle strip); the per-instance attribute carries the tree's
// world base position + uniform scale. The quad faces the camera with WORLD-UP orientation so it lines
// up with the octahedral atlas tiles (each baked with up = +Y).

layout(location = 0) in mat4 iModel;
layout(location = 4) in vec4 iTint;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec3 u_cameraPos;
uniform float u_radius;   // tree local bounding-sphere radius
uniform vec3 u_center;   // tree local bounding-sphere center

out vec2 vQuadUV;   // [0,1] within-tile UV (the billboard surface)
out vec3 vViewDir;  // world direction tree -> camera (selects the octa tile)
out vec3 vWorldPos;
flat out mat3 vObjectToWorld;
flat out vec3 vTint;
out vec3 vViewPos;  // view-space position (gPosition attachment)

void main() {
    // Triangle-strip quad corners.
    vec2 corners[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec2 c = corners[gl_VertexID];

    vec3 treeCenter = (iModel * vec4(u_center, 1.0)).xyz;
    vec3 scale = vec3(length(iModel[0].xyz), length(iModel[1].xyz), length(iModel[2].xyz));
    mat3 orientation = mat3(iModel[0].xyz / scale.x, iModel[1].xyz / scale.y, iModel[2].xyz / scale.z);
    float halfSize = u_radius * max(scale.x, max(scale.y, scale.z));

    vec3 vd = normalize(u_cameraPos - treeCenter);
    vec3 up = orientation[1];
    vec3 right = cross(up, vd);
    float rl = length(right);
    right = (rl < 0.01) ? vec3(1.0, 0.0, 0.0): right / rl; // top-down fallback
    vec3 bUp = normalize(cross(vd, right));                 // billboard up tilts toward world-up

    vec3 worldPos = treeCenter + right * (c.x * halfSize) + bUp * (c.y * halfSize);
    gl_Position = u_proj * u_view * vec4(worldPos, 1.0);

    vQuadUV = c * 0.5 + 0.5;
    vViewDir = transpose(orientation) * vd;
    vObjectToWorld = orientation;
    vTint = iTint.rgb;
    vWorldPos = worldPos;
    vViewPos = (u_view * vec4(worldPos, 1.0)).xyz;
}
