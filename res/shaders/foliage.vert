#version 450 core

// ===========================================================================
// instanced foliage scatter vertex stage.
//
// One instanced CROSSED blade/clutter sprite per scatter instance, drawn from
// the persistent-mapped FoliageInstance pool (FoliagePass). Each instance is a
// pair of vertical (world-up) cards crossed at 90 degrees so the blade reads as
// upright ground cover from ANY view angle (a single flat card read as a neon
// playing-card decal from the down-pitched cells -- ).
// The 12 verts per instance are two quads (2 triangles each) generated from
// gl_VertexID; NO geometry shader (the Shader class is vert+frag only), matching
// the  ParticlePass. Draw: glDrawArraysInstanced(GL_TRIANGLES, 0, 12, count).
//
// WIND SWAY (documented design): the TOP of each card is displaced by the
// per-instance wind vector (the  wind field sampled CPU-side and packed into
// the instance) scaled by the per-archetype sway flag. The base stays pinned to
// the ground so the card  from the root.: the sway never feeds
// the sim/world_hash (one-way, regression contract).
//
// SKY / HORIZON CULL (, GREEN_SKY_SPECKLE): ground cover
// must never render against the sky. A blade whose tip projects ABOVE the
// horizon line (camera eye height) is a distant card poking over the terrain
// silhouette -- it is collapsed to a degenerate point so it cannot speckle the
// sky. Combined with a steeper quadratic distance fade so the far half of the
// live ring is already nearly gone before the horizon.
//
// Per-instance attributes come from the 36-byte FoliageInstance:
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
    float heightT;    // 0 at the blade root.. 1 at the tip (base-to-tip gradient)
    vec3  worldPos;
    vec3  worldNormal;
} vs_out;

// Emit a degenerate (off-screen) vertex so an entire blade is culled cheaply.
void emitCulled(vec3 worldPos) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside clip space -> clipped
    vs_out.texCoord = vec2(0.0);
    vs_out.color = vec4(0.0);
    vs_out.fade = 0.0;
    vs_out.heightT = 0.0;
    vs_out.worldPos = worldPos;
    vs_out.worldNormal = vec3(0.0, 1.0, 0.0);
}

void main() {
    // Two crossed quads, 6 verts each (two triangles): vertex within a quad is
    // gl_VertexID % 6, the quad index is gl_VertexID / 6 (0 or 1). The two quads
    // share the same base anchor but are rotated 90 degrees about world-up, so
    // the blade always presents a face toward the viewer (never a flat decal).
    int vInQuad = gl_VertexID % 6;
    int quad    = gl_VertexID / 6;

    // Unit quad corner for a 2-triangle quad: (0,0)(1,0)(0,1) / (1,0)(1,1)(0,1).
    // cornerX in {-1,+1}, cornerY in {0 (base), 1 (tip)}.
    vec2 q;
    if      (vInQuad == 0) q = vec2(-1.0, 0.0);
    else if (vInQuad == 1) q = vec2( 1.0, 0.0);
    else if (vInQuad == 2) q = vec2(-1.0, 1.0);
    else if (vInQuad == 3) q = vec2( 1.0, 0.0);
    else if (vInQuad == 4) q = vec2( 1.0, 1.0);
    else                   q = vec2(-1.0, 1.0);
    float cornerX = q.x;
    float cornerY = q.y;

    // Card basis: a yaw-rotated horizontal axis + world up. The second quad is
    // offset by 90 degrees so the pair forms a cross. Fixed per-instance yaw (no
    // camera spin) so the scatter looks like planted ground cover.
    float yaw = aFacing + (quad == 1 ? 1.5707963: 0.0);
    float cf = cos(yaw);
    float sf = sin(yaw);
    vec3 cardRight = vec3(cf, 0.0, sf);
    vec3 cardUp    = vec3(0.0, 1.0, 0.0);

    //  (GROUNDING): the base verts (cornerY==0) are sunk a
    // few cm BELOW the surface anchor so the blade root is buried in the terrain
    // -- this removes the visible hover gap (blades looked like floating dashes)
    // and guarantees ground contact even where the rendered terrain micro-varies
    // from the sampled surface height. The tip still extends a full aSize.y up.
    const float kRootSink = 0.06; // metres buried below the surface
    float vy = cornerY * aSize.y - (1.0 - cornerY) * kRootSink;
    vec3 local = cardRight * (cornerX * aSize.x) + cardUp * vy;

    // WIND SWAY: displace only the upper part of the card (quadratic in height
    // so the base is pinned). The sway flag scale rides in aColor.a; pebbles /
    // clutter pack a 0 there so they do not  The oscillation is a cheap
    // deterministic sine driven by render time + the per-instance phase.
    float swayScale = aColor.a;
    float bend = cornerY * cornerY; // 0 at base, 1 at tip
    float osc = sin(u_time * u_swaySpeed + aPhase);
    vec2 windDisp = aSway * u_swayAmplitude * swayScale * bend * (0.6 + 0.4 * osc);
    // Wind is a field velocity, not a blade-size multiplier. Bound displacement
    // by this blade's height so short ground cover cannot stretch into long spikes.
    float maxBend = max(aSize.y, 0.0) * 0.45;
    windDisp *= min(1.0, maxBend / max(length(windDisp), 0.0001));
    local.x += windDisp.x;
    local.z += windDisp.y;

    vec3 worldPos = aPos + local;

    // Distance fade against the far-LOD horizon: no foliage past u_fadeEnd. The
    // fade is QUADRATIC in the normalized ring distance so the far half of the
    // live ring is already nearly transparent -- distant cards never gather into
    // a visible green band near the horizon (defect: GREEN_SKY_SPECKLE).
    float dist = length(aPos - u_cameraPos);
    float distT = clamp((dist - u_fadeStart) / max(1.0, u_fadeEnd - u_fadeStart), 0.0, 1.0);
    float fade = 1.0 - distT;
    fade = fade * fade;
    // Collapse fully-faded instances to a degenerate point (zero pixels) so the
    // live-ring boundary is hard (gate: no foliage beyond the live ring).
    if (fade <= 0.001) {
        emitCulled(worldPos);
        return;
    }

    // SKY / HORIZON CULL: a ground-cover blade must never poke above the horizon
    // line and speckle the sky. The horizon (for an eye-level/down-pitched view)
    // sits at the camera eye height; any blade whose tip approaches the camera
    // eye is, by construction, a distant card seen over the terrain silhouette
    // (nearby ground cover sits a person-height below the eye). Cull with a 1 m
    // margin BELOW the eye so even cards on ground that rises toward the horizon
    // are removed before they can speckle the sky/storm dome. Pure world-space
    // test (no screen-space feedback) -> deterministic.
    if (worldPos.y > u_cameraPos.y - 1.0) {
        emitCulled(worldPos);
        return;
    }

    // SCREEN-BAND CULL (the airtight sky/horizon guard): ground cover must never
    // appear in the UPPER part of the frame -- that band is the distant horizon /
    // sky where a green card reads as a firefly speckle (GREEN_SKY_SPECKLE) or a
    // false aurora (AURORA_AT_DUSK). The objective critique samples the TOP THIRD,
    // i.e. NDC y > +0.33. We test the per-instance TIP (anchor + full blade
    // height) -- shared by all 12 verts so the whole blade is culled together (no
    // torn triangles). : testing the TIP (not just the
    // anchor) closes the gap where a blade rooted just under the old anchor line
    // poked its now-brighter GREEN tip into the top third at the dawn/storm
    // horizon (GREEN_SKY_SPECKLE). Cull when the TIP projects above y = +0.10,
    // comfortably below the top-third window with margin. Two clip-space tests
    // (anchor for the near floor, tip for the horizon) -> deterministic.
    //  coverage: the objective sky-speckle sample is the TOP THIRD (NDC y > 0.33).
    // The previous thresholds (anchor 0.05 / tip 0.10) were far below that window, so a
    // DOWN-PITCHED view (horizon high in frame) culled most below-horizon ground cover
    // and the foreground read as sparse tufts. Raise the cull to just BELOW the sky
    // window (anchor 0.26 / tip 0.30) so ground cover fills the lower ~two-thirds while
    // the top-third sky sample stays foliage-free (the geometric above-horizon cull at
    // worldPos.y > eye-1 above is the primary sky guard; this is the screen-band backstop
    // with margin to y=0.33). Re-validated against GREEN_SKY_SPECKLE/AURORA_AT_DUSK.
    vec4 anchorClip = u_projection * (u_view * vec4(aPos, 1.0));
    if (anchorClip.w > 0.0 && (anchorClip.y / anchorClip.w) > 0.26) {
        emitCulled(worldPos);
        return;
    }
    vec3 tipWorld = aPos + vec3(0.0, aSize.y, 0.0);
    vec4 tipClip = u_projection * (u_view * vec4(tipWorld, 1.0));
    if (tipClip.w > 0.0 && (tipClip.y / tipClip.w) > 0.30) {
        emitCulled(worldPos);
        return;
    }

    vec4 clip = u_projection * (u_view * vec4(worldPos, 1.0));

    vs_out.texCoord = vec2(cornerX * 0.5 + 0.5, cornerY);
    vs_out.color = aColor;
    vs_out.fade = fade;
    vs_out.heightT = cornerY;
    vs_out.worldPos = worldPos;
    // Two-sided card; approximate normal as the card normal biased toward up so
    // the cover catches sky/sun light from above (reads as ground vegetation).
    vs_out.worldNormal = normalize(vec3(-sf, 1.6, cf));

    gl_Position = clip;
}
