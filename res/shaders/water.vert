#version 450 core

layout (location = 0) in vec3 a_pos;
layout (location = 1) in vec3 a_normal;

// Uniforms for transforming the chunk mesh
uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform float u_time; // shared with the fragment stage (set once on the program)

// Outputs for the fragment shader
out VS_OUT {
    vec3 world_pos;
    vec3 world_normal;
} vs_out;

// ===========================================================================
//  #3: gentle Gerstner SWELLS.
//
// The water surface was a perfectly flat per-chunk mesh; all surface motion
// lived in the fragment shader's per-pixel ripple NORMALS, so the silhouette
// never moved and grazing views read as dead-flat glass. This adds a small sum
// of Gerstner trochoids that displace the surface VERTICES (horizontal pinch +
// vertical lift) and supplies the matching ANALYTIC normal, so the water now
// has real rolling relief. The fragment ripple field stays on top for micro
// detail.
//
// ZEN-CALM tuning (owner: "gentle, zen-calm"): long  (11..26 m), low
// amplitudes (sum < ~0.32 m), low steepness (no curling whitecaps), slow phase.
// The 2 m water-mesh vertex spacing (CHUNK_SIZE 16 / resolution 8) gives 5..13
// verts per  -> smooth undulation, not facets.
//
// this displaces ONLY the rendered surface mesh. The sim water
// level (collision, depth queries, world_hash) is untouched; nothing here feeds
// back into any sim state. Pure function of (world XZ, u_time).
// ===========================================================================

// A single Gerstner wave: direction D (unit, XZ plane),  L, amplitude
// A, steepness Q (0..1 share of the max non-self-intersecting pinch), phase
// speed scalar. Accumulates the displacement and the analytic normal partials.
void gerstner_wave(vec2 D, float L, float A, float Q, float speed,
                   vec2 base_xz, float t,
                   inout vec3 disp, inout vec3 nrm)
{
    float w = 6.2831853 / L;          // angular  (2*pi / )
    float phase = w * dot(D, base_xz) + speed * w * t;
    float c = cos(phase);
    float s = sin(phase);
    float WA = w * A;

    // Horizontal pinch toward the crest + vertical lift.
    disp.x += Q * A * D.x * c;
    disp.z += Q * A * D.y * c;
    disp.y += A * s;

    // Analytic surface normal partials (GPU Gems 1, ch. 1).
    nrm.x -= D.x * WA * c;
    nrm.z -= D.y * WA * c;
    nrm.y -= Q * WA * s;
}

void main()
{
    // Position in world space (for lighting and world-based effects).
    vec4 world_pos_4 = u_model * vec4(a_pos, 1.0);
    vec2 base_xz = world_pos_4.xz; // undisplaced world XZ drives the

    vec3 disp = vec3(0.0);
    vec3 nrm = vec3(0.0, 1.0, 0.0); // start from the flat up-normal; partials below

    // A small calm swell spectrum: one long primary swell + two shorter
    // cross-swells for natural interference. Directions spread around the
    // compass so the pattern does not read as a single marching ridge.
    gerstner_wave(normalize(vec2( 1.00,  0.22)), 26.0, 0.16, 0.16, 0.55, base_xz, u_time, disp, nrm);
    gerstner_wave(normalize(vec2( 0.55, -0.84)), 17.0, 0.095, 0.22, 0.72, base_xz, u_time, disp, nrm);
    gerstner_wave(normalize(vec2(-0.72,  0.50)), 11.0, 0.055, 0.26, 0.95, base_xz, u_time, disp, nrm);

    world_pos_4.xyz += disp;
    vs_out.world_pos = world_pos_4.xyz;

    // The Gerstner normal is computed in world space already; fold in the mesh's
    // model rotation only if present (water chunks use a pure translation model,
    // so the analytic normal is world-space as-is). Normalize for safety.
    vs_out.world_normal = normalize(nrm);

    // Final clip-space position from the DISPLACED world position.
    gl_Position = u_projection * u_view * world_pos_4;
}
