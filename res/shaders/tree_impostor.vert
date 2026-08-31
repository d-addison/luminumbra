#version 450 core
//  far-field tree impostor. One camera-facing billboard quad per distant tree, drawn instanced.
// gl_VertexID gives the quad corner (triangle strip); the per-instance attribute carries the tree's
// world base position + uniform scale. The quad faces the camera with WORLD-UP orientation so it lines
// up with the octahedral atlas tiles (each baked with up = +Y).

layout(location = 0) in vec4 iPosScale; // xyz = tree base world position, w = scale (per-instance, divisor 1)

uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec3 u_cameraPos;
uniform float u_radius;   // tree local bounding-sphere radius
uniform float u_sphereY;  // tree local bounding-sphere centre height

out vec2 vQuadUV;   // [0,1] within-tile UV (the billboard surface)
out vec3 vViewDir;  // world direction tree -> camera (selects the octa tile)
out vec3 vWorldPos;
out vec3 vViewPos;  // view-space position (gPosition attachment)

void main() {
    // Triangle-strip quad corners.
    vec2 corners[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec2 c = corners[gl_VertexID];

    vec3 treeCenter = iPosScale.xyz + vec3(0.0, u_sphereY * iPosScale.w, 0.0);
    float halfSize = u_radius * iPosScale.w;

    vec3 vd = normalize(u_cameraPos - treeCenter);
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 right = cross(vd, up);
    float rl = length(right);
    right = (rl < 0.01) ? vec3(1.0, 0.0, 0.0): right / rl; // top-down fallback
    vec3 bUp = normalize(cross(right, vd));                 // billboard up tilts toward world-up

    vec3 worldPos = treeCenter + right * (c.x * halfSize) + bUp * (c.y * halfSize);
    gl_Position = u_proj * u_view * vec4(worldPos, 1.0);

    vQuadUV = c * 0.5 + 0.5;
    vViewDir = vd;
    vWorldPos = worldPos;
    vViewPos = (u_view * vec4(worldPos, 1.0)).xyz;
}
