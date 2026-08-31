#version 450 core

// ===========================================================================
// GPU particle framework vertex stage.
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
// location 4 is now an snorm8 rotation (angle/pi in
// [-1,1]) and location 5 a unorm8 streak aspect (aspect/16 in [0,1]). The streak
// aspect ELONGATES the velocity-aligned billboard so rain renders as a vertical
// (wind-sheared) streak instead of a round dot.
layout (location = 4) in float aRotation;   // normalized: angle = aRotation * PI
layout (location = 5) in float aStreak;      // normalized: aspect = aStreak * 16

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
    flat float streakAspect;  // >1 = elongated rain streak; 1 = round sprite
} vs_out;

const float PI_PARTICLE = 3.14159265358979323846;
const float MAX_STREAK_ASPECT = 16.0;

void main() {
    // Quad corner from gl_VertexID for a triangle strip:
    //   0 -> (-1,-1), 1 -> (+1,-1), 2 -> (-1,+1), 3 -> (+1,+1)
    vec2 corner = vec2(
        (gl_VertexID == 1 || gl_VertexID == 3) ? 1.0: -1.0,
        (gl_VertexID == 2 || gl_VertexID == 3) ? 1.0: -1.0);

    // Decode the packed rotation (snorm8 angle/pi) + streak aspect (unorm8).
    float angle = aRotation * PI_PARTICLE;
    float aspect = max(1.0, aStreak * MAX_STREAK_ASPECT);

    // ELONGATE the local quad along its streak axis
    // (local Y) by the aspect, and SLIM it across (local X) so the on-screen area
    // stays modest -- a thin tall streak rather than a fat dot. The subsequent
    // rotation aligns the streak to the screen-projected velocity (set on the CPU
    // as atan2(horiz, vert)), so a wind-slanted velocity renders a slanted streak.
    float widthScale = (aspect > 1.0) ? (1.0 / sqrt(aspect)): 1.0;
    vec2 shaped = vec2(corner.x * widthScale, corner.y * aspect);

    // Rotate the (elongated) billboard in its own plane.
    float c = cos(angle);
    float s = sin(angle);
    vec2 rotated = vec2(shaped.x * c - shaped.y * s,
                        shaped.x * s + shaped.y * c);

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
    vs_out.streakAspect = aspect;

    gl_Position = u_projection * viewPos;
}
