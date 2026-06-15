#version 450 core

// ===========================================================================
// T-I5b-1 (F1): instanced foliage scatter vertex stage.
//
// One instanced quad-strip blade/clutter sprite per scatter instance, drawn
// from the persistent-mapped FoliageInstance pool (FoliagePass). Each instance
// is a vertical (world-up) billboarded card so grass/clutter read as upright
// ground cover. Four corners are generated from gl_VertexID (a triangle strip:
// glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instanceCount)); NO geometry
// shader (the Shader class is vert+frag only), matching the A1 ParticlePass.
//
// WIND SWAY (design-decisions §2): the TOP of the card is displaced by the
// per-instance wind vector (the A2 wind field sampled CPU-side and packed into
// the instance) scaled by the per-archetype sway flag. The base stays pinned to
// the ground so the card waves from the root. RENDER-ONLY: the sway never feeds
// the sim/world_hash (one-way, critique F2).
//
// Per-instance attributes come from the 32-byte FoliageInstance:
//   0: pos (vec3)        ground anchor (world)
//   1: size (vec2)       half-width / height (world units)
//   2: color (rgba8)     albedo tint (a = sway flag scale 0..1)
//   3: sway (vec2)       per-instance wind displacement (world XZ at the tip)
//   4: phase (f16)       per-instance sway phase offset (radians)
//   5: facing (f16)      yaw of the card in the XZ plane (radians)
// ===========================================================================

layout (location = 0) in vec3  aPos;
layout (location = 1) in vec2  aSize;
layout (location = 2) in vec4  aColor;
layout (location = 3) in vec2  aSway;
layout (location = 4) in float aPhase;
layout (location = 5) in float aFacing;

uniform mat4  u_view;
uniform mat4  u_projection;
uniform vec3  u_cameraPos;
uniform float u_time;
uniform float u_swayAmplitude; // global sway strength multiplier
uniform float u_swaySpeed;     // global sway oscillation speed
uniform float u_fadeStart;     // world distance where foliage begins fading
uniform float u_fadeEnd;       // world distance where foliage is fully gone

out VS_OUT {
    vec2  texCoord;
    vec4  color;
    float fade;       // [0,1] distance fade (0 == culled at the tip)
    vec3  worldPos;
    vec3  worldNormal;
} vs_out;

void main() {
    // Quad corner from gl_VertexID for a triangle strip:
    //   0 -> (-1, 0) base-left, 1 -> (+1, 0) base-right,
    //   2 -> (-1, 1) tip-left,  3 -> (+1, 1) tip-right.
    float cornerX = (gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0;
    float cornerY = (gl_VertexID == 2 || gl_VertexID == 3) ? 1.0 : 0.0; // 0 base .. 1 tip

    // Card basis: a yaw-rotated horizontal axis + world up. The card faces a
    // fixed per-instance yaw (deterministic) rather than the camera, so the
    // scatter looks like real ground cover, not billboards spinning to face you.
    float cf = cos(aFacing);
    float sf = sin(aFacing);
    vec3 cardRight = vec3(cf, 0.0, sf);
    vec3 cardUp    = vec3(0.0, 1.0, 0.0);

    vec3 local = cardRight * (cornerX * aSize.x) + cardUp * (cornerY * aSize.y);

    // WIND SWAY: displace only the upper part of the card (quadratic in height
    // so the base is pinned). The sway flag scale rides in aColor.a; pebbles /
    // clutter pack a 0 there so they do not wave. The oscillation is a cheap
    // deterministic sine driven by render time + the per-instance phase.
    float swayScale = aColor.a;
    float bend = cornerY * cornerY; // 0 at base, 1 at tip
    float osc = sin(u_time * u_swaySpeed + aPhase);
    vec2 windDisp = aSway * u_swayAmplitude * swayScale * bend * (0.6 + 0.4 * osc);
    local.x += windDisp.x;
    local.z += windDisp.y;

    vec3 worldPos = aPos + local;

    // Distance fade against the far-LOD horizon: no foliage past u_fadeEnd.
    float dist = length(aPos - u_cameraPos);
    float fade = 1.0 - clamp((dist - u_fadeStart) / max(1.0, u_fadeEnd - u_fadeStart), 0.0, 1.0);
    // Collapse fully-faded instances to a degenerate point (zero pixels) so the
    // live-ring boundary is hard (gate: no foliage beyond the live ring).
    if (fade <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside clip space -> culled
        vs_out.texCoord = vec2(0.0);
        vs_out.color = vec4(0.0);
        vs_out.fade = 0.0;
        vs_out.worldPos = worldPos;
        vs_out.worldNormal = vec3(0.0, 1.0, 0.0);
        return;
    }

    vs_out.texCoord = vec2(cornerX * 0.5 + 0.5, cornerY);
    vs_out.color = aColor;
    vs_out.fade = fade;
    vs_out.worldPos = worldPos;
    // Two-sided card; approximate normal as the card normal biased toward up.
    vs_out.worldNormal = normalize(vec3(-sf, 1.2, cf));

    gl_Position = u_projection * (u_view * vec4(worldPos, 1.0));
}
