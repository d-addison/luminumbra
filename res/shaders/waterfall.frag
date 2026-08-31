#version 450 core

// ===========================================================================
// waterfall falling-sheet shader.  DRESSING.
//
// An animated vertical sheet of falling water drawn over a detected waterfall
// site (WaterfallDetect). The sheet uses a procedural FLOW-MAP: vertical streaks
// scroll DOWNWARD over time (the flow direction), with layered high-frequency
// turbulence so the cascade reads as broken, frothy water rather than a flat
// scrolling band. The crest and the plunge foot are foamed white; the mid-fall
// is a bright blue-white cascade. No external textures are required (the flow is
// generated), so the gate can compile + render this standalone.
//
// Inputs are the basic.vert outputs (FragPos = world position, Normal). The
// sheet quad is oriented so its local "down" is world -Y; the shader keys the
// flow on world Y (height down the fall) and the horizontal channel coordinate.
// ===========================================================================

in vec3 FragPos;
in vec3 Normal;

out vec4 o_frag_color;

uniform float u_time;
uniform vec3  u_camera_pos;
// The fall extent in world space so the shader can normalize height-down-the-
// fall to [0,1] (0 = crest, 1 = plunge foot) for the foam bands.
uniform float u_crest_y;   // world Y of the lip
uniform float u_foot_y;    // world Y of the plunge pool
uniform vec3  u_sun_color; // tint for the lit froth (defaults handled by caller)
// Scene-light multiplier (sun intensity + ambient floor) so the cascade is LIT by the
// scene instead of emitting near-white. DEFAULTS to 1.0 so the standalone WaterfallVisual
// gate (which never sets it) renders byte-identically; the live pipeline sets it from the
// time-of-day sun intensity so the fall darkens at dusk/night..
uniform vec3  u_scene_light = vec3(1.0);
//  ( T.1): the LIVE upstream water factor [0,1] — 1 = the full
// authored sheet; ->0 = upstream dammed/drained, the veil thins out. Default 1
// keeps the standalone WaterfallVisual gate (which never sets it) byte-stable.
uniform float u_live_factor = 1.0;

// Cheap hash + value noise for the procedural flow turbulence.
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    // Height down the fall, normalized [0,1] (0 = crest, 1 = foot).
    float span = max(0.001, u_crest_y - u_foot_y);
    float fall_t = clamp((u_crest_y - FragPos.y) / span, 0.0, 1.0);

    // Horizontal channel coordinate (world XZ projected): gives each vertical
    // streak a stable lane so streaks don't smear sideways.
    float lane = FragPos.x * 0.7 + FragPos.z * 0.7;

    // --- FLOW MAP: vertical streaks scrolling DOWN over time. ---
    // The dominant scroll term is u_time on the height axis (water falls), with
    // two octaves of turbulence at different speeds so the sheet churns.
    float scroll = u_time * 1.8;
    float streak1 = value_noise(vec2(lane * 1.3, fall_t * 9.0 + scroll));
    float streak2 = value_noise(vec2(lane * 3.1 + 5.0, fall_t * 18.0 + scroll * 1.7));
    float streak3 = value_noise(vec2(lane * 6.2 - 3.0, fall_t * 30.0 + scroll * 2.4));
    float turbulence = streak1 * 0.55 + streak2 * 0.30 + streak3 * 0.15;

    // Vertical streak structure: sharpen the noise into bright filaments so the
    // cascade reads as falling threads of water.
    float streaks = pow(turbulence, 1.3);

    // --- Colour: AERATED blue-white cascade so it reads as falling water, NOT
    // the calm blue lake surface behind it. The body is a bright churned blue and
    // the streaks lift to near-white froth. ---
    vec3 aerated_body = vec3(0.55, 0.74, 0.88);  // bright churned water (lighter than the lake)
    vec3 bright_foam  = vec3(0.95, 0.98, 1.0);
    vec3 cascade = mix(aerated_body, bright_foam, clamp(streaks * 1.6, 0.0, 1.0));

    // --- Foam bands at the crest and the plunge foot (widened so they read). ---
    // Crest froth (top ~18%) and plunge-pool foam (bottom ~30%) churn white.
    float crest_foam = smoothstep(0.18, 0.0, fall_t);
    float plunge_foam = smoothstep(0.70, 1.0, fall_t);
    // Animate the plunge foam so the pool roils.
    float roil = 0.5 + 0.5 * value_noise(vec2(lane * 4.0, u_time * 2.2));
    plunge_foam *= (0.6 + 0.4 * roil);

    float foam = clamp(crest_foam + plunge_foam, 0.0, 1.0);
    cascade = mix(cascade, bright_foam, foam);

    // Light the cascade by the SCENE (sun colour+intensity via u_scene_light) instead of
    // emitting near-white. The 0.25 sun TINT is kept for froth warmth; u_scene_light brings
    // the whole sheet down to scene exposure (and to near-black at night) — the bright-
    // waterfall fix.
    vec3 lit = cascade * mix(vec3(1.0), u_sun_color, 0.25) * u_scene_light;

    // Sheet opacity: the brightness/"too bright" complaint is now handled by u_scene_light
    // (scene exposure), so keep the veil clearly VISIBLE as falling water — a solid floor so it
    // reads against the cliff instead of vanishing, denser at the foam/streaks.
    float alpha = clamp(0.66 + streaks * 0.30 + foam * 0.30, 0.66, 0.97);

    // a starving fall thins toward transparent (the CPU side skips the
    // draw entirely below 0.02, so this only shades partially-starved sheets).
    alpha *= clamp(u_live_factor, 0.0, 1.0);

    o_frag_color = vec4(lit, alpha);
}
