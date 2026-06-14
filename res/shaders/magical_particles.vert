#version 450 core

// ===========================================================================
// T-I5a-1: GPU particle framework vertex stage.
//
// Re-home of the magical_particles billboard onto the instanced, fixed-capacity
// persistent-mapped pool (ParticlePass). Each particle is drawn as an instanced
// quad: glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instanceCount). The four
// corners are generated from gl_VertexID, so NO geometry shader is used by the
// live path (the Shader class is vert+frag only).
//
// The legacy magical_particles.geom is RETAINED so the render shader inventory
// (test/rendering/render_smoke_test.cpp) can still compile + link a
// vert+geom+frag program for the magical_particles entry. The output interface
// block below (VS_OUT) is shared by both the 2-stage (vert->frag) live link and
// the 3-stage (vert->geom->frag) inventory link, so both link cleanly.
//
// Per-instance attributes come from the 24-byte InstanceRecord:
//   0: pos (vec3), 1: size (float), 2: color (rgba8 -> vec4),
//   3: atlasLayer (uint16 -> uint), 4: rotation (f16 -> float).
// ===========================================================================

layout (location = 0) in vec3  aPos;
layout (location = 1) in float aSize;
layout (location = 2) in vec4  aColor;
layout (location = 3) in uint  aAtlasLayer;
layout (location = 4) in float aRotation;

uniform mat4  u_view;
uniform mat4  u_projection;
uniform vec3  u_cameraRight;
uniform vec3  u_cameraUp;
uniform vec3  u_cameraPos;
uniform float u_time;
uniform vec2  u_screenSize;
uniform float u_nearPlane;
uniform float u_farPlane;

// Shared interface block. Consumed directly by the fragment stage in the live
// (vert+frag) path, and re-emitted by the geometry stage in the inventory link.
out VS_OUT {
    vec2  texCoord;
    vec4  color;
    flat float atlasLayer;
    float distanceToCamera;
    float viewDepth;   // positive linear view-space depth of the billboard centre
    vec3  worldPos;    // billboard corner world position (soft-particle depth)
} vs_out;

void main() {
    // Quad corner from gl_VertexID for a triangle strip:
    //   0 -> (-1,-1), 1 -> (+1,-1), 2 -> (-1,+1), 3 -> (+1,+1)
    vec2 corner = vec2(
        (gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
        (gl_VertexID == 2 || gl_VertexID == 3) ? 1.0 : -1.0);

    // Rotate the billboard in its own plane.
    float c = cos(aRotation);
    float s = sin(aRotation);
    vec2 rotated = vec2(corner.x * c - corner.y * s,
                        corner.x * s + corner.y * c);

    vec3 right = normalize(u_cameraRight);
    vec3 up    = normalize(u_cameraUp);
    vec3 worldCorner = aPos + (right * rotated.x + up * rotated.y) * aSize;

    vec4 viewPos = u_view * vec4(worldCorner, 1.0);

    vs_out.texCoord = corner * 0.5 + 0.5;
    vs_out.color = aColor;
    vs_out.atlasLayer = float(aAtlasLayer);
    vs_out.distanceToCamera = length(aPos - u_cameraPos);
    vs_out.viewDepth = -viewPos.z;
    vs_out.worldPos = worldCorner;

    gl_Position = u_projection * viewPos;
}
