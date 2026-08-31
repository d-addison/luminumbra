#version 450 core

// ===========================================================================
// magical_particles geometry stage (RETAINED for the shader inventory).
//
// The LIVE particle path (ParticlePass) expands billboards in the vertex stage
// via gl_VertexID and does NOT bind a geometry shader. This geometry stage is
// kept so the render shader inventory (test/rendering/render_smoke_test.cpp)
// can still compile + link a vert+geom+frag program for the magical_particles
// entry, and so the ShaderInventory gate's required geometry-stage assertion
// stays green. It mirrors the shared VS_OUT/GS_OUT interface block so the
// 3-stage link succeeds; it is a point->quad billboard expander.
// ===========================================================================

layout (points) in;
layout (triangle_strip, max_vertices = 4) out;

uniform mat4  u_view;
uniform mat4  u_projection;
uniform vec3  u_cameraRight;
uniform vec3  u_cameraUp;
uniform float u_time;

in VS_OUT {
    vec2  texCoord;
    vec4  color;
    flat float atlasLayer;
    float distanceToCamera;
    float viewDepth;
    vec3  worldPos;
    flat float streakAspect;
} gs_in[];

out VS_OUT {
    vec2  texCoord;
    vec4  color;
    flat float atlasLayer;
    float distanceToCamera;
    float viewDepth;
    vec3  worldPos;
    flat float streakAspect;
} gs_out;

void emit_corner(vec4 centre, vec2 corner) {
    vec3 right = normalize(u_cameraRight);
    vec3 up    = normalize(u_cameraUp);
    vec3 offset = (right * corner.x + up * corner.y);
    gl_Position = centre + u_projection * u_view * vec4(offset, 0.0);
    gs_out.texCoord = corner * 0.5 + 0.5;
    gs_out.color = gs_in[0].color;
    gs_out.atlasLayer = gs_in[0].atlasLayer;
    gs_out.distanceToCamera = gs_in[0].distanceToCamera;
    gs_out.viewDepth = gs_in[0].viewDepth;
    gs_out.worldPos = gs_in[0].worldPos;
    gs_out.streakAspect = gs_in[0].streakAspect;
    EmitVertex();
}

void main() {
    vec4 centre = gl_in[0].gl_Position;
    emit_corner(centre, vec2(-1.0, -1.0));
    emit_corner(centre, vec2( 1.0, -1.0));
    emit_corner(centre, vec2(-1.0,  1.0));
    emit_corner(centre, vec2( 1.0,  1.0));
    EndPrimitive();
}
