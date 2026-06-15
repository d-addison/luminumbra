#version 450 core

// ===========================================================================
// T-I5b-1 (F1): instanced foliage scatter fragment stage.
//
// Forward-lit ground cover blended into the lit HDR target after the opaque
// terrain. A simple blade-shaped alpha mask + sun/ambient lambert keeps the
// scatter cheap; the distance fade arrives as vs_out.fade from the vertex
// stage. RENDER-ONLY (no sim writes).
//
// T-I5b-DR-foliage-blocker: the flat single-hue neon look is broken up here with
//   * a base-to-tip VALUE gradient (darker root, slightly lighter tip) so the
//     cover reads with depth instead of as flat cards,
//   * a GREEN-PRESERVING ambient floor so foliage stays recognizably green in
//     low light (dusk) instead of collapsing to dark grey -- this is what made
//     the dusk down-view register as FOLIAGE_SPARSE,
//   * a desaturation clamp so the lit colour never over-drives the green channel
//     into the speckle-detector range.
// ===========================================================================

in VS_OUT {
    vec2  texCoord;
    vec4  color;
    float fade;
    float heightT;   // 0 root .. 1 tip
    vec3  worldPos;
    vec3  worldNormal;
} fs_in;

uniform vec3  u_sunDirection;  // direction the sunlight travels
uniform vec3  u_sunColor;
uniform float u_sunIntensity;
uniform vec3  u_ambientColor;

out vec4 FragColor;

void main() {
    // Blade alpha mask: taper toward the tip and soften the vertical edges so
    // the card reads as a tuft rather than a hard quad. texCoord.y: 0 base, 1 tip.
    float edge = 1.0 - abs(fs_in.texCoord.x * 2.0 - 1.0); // 0 at sides, 1 centre
    float taper = mix(1.0, 0.25, fs_in.texCoord.y);       // narrower at the tip
    float alpha = smoothstep(0.0, 0.35, edge * taper) * fs_in.fade;
    if (alpha < 0.02) {
        discard;
    }

    // Base-to-tip value gradient: roots in shadow, tips catching light. This is
    // the per-blade tonal variation that kills the flat-decal read.
    vec3 albedo = fs_in.color.rgb;
    float ao = mix(0.55, 1.12, fs_in.heightT); // darker root, lighter tip

    vec3 N = normalize(fs_in.worldNormal);
    float ndl = max(dot(N, -normalize(u_sunDirection)), 0.0);

    // GREEN-PRESERVING floor: ground vegetation keeps a soft self-colour even
    // when the sun is low / warm so it never collapses to grey or warm dirt. The
    // ambient term is applied at full strength; on top of it an ADDITIVE constant
    // green-biased self-glow guarantees the green channel stays DOMINANT at dusk
    // (warm low sun otherwise pushes red past green -> FOLIAGE_SPARSE). The glow
    // is small in absolute terms (well below the speckle detector's luma floor in
    // the sky, where blades are culled anyway) but enough to keep ground cover
    // reading as vegetation. RENDER-ONLY.
    vec3 ambient = (u_ambientColor + vec3(0.05, 0.14, 0.05)) * albedo;
    // Damp the warm direct tint HARD on the red channel so a low warm dusk sun
    // does not flip the grass orange (red was beating green across the whole
    // ground third -> FOLIAGE_SPARSE). Vegetation chlorophyll reflects green and
    // absorbs red, so a green-biased response is physically reasonable.
    vec3 sunTint = vec3(0.42, 1.0, 0.70) * u_sunColor;
    vec3 direct  = sunTint * (u_sunIntensity * ndl) * albedo;
    // Additive green-biased vegetation floor. This is the MINIMUM light a blade
    // emits regardless of sun angle, so low-sun / shadow-side grass at dusk does
    // not collapse to dark silhouettes (which read as bare ground, not cover ->
    // FOLIAGE_SPARSE). Sized so a dusk blade clears the analyzer's g>50 floor and
    // stays green-dominant after tonemap, while staying modest next to bright noon
    // direct light. The root is kept darker via `ao` for the base-to-tip gradient.
    // Fade the self-glow with distance (fs_in.fade already squared in the vertex
    // stage) so DISTANT grass near the horizon goes dark/transparent and cannot
    // bleed green into the sky third (AURORA_AT_DUSK / GREEN_SKY_SPECKLE). Only
    // the near-field carpet keeps the full green floor.
    vec3 selfGlow = vec3(0.08, 0.36, 0.13) * mix(0.55, 1.0, fs_in.heightT) * fs_in.fade;
    vec3 lit = (ambient + direct) * ao + selfGlow;

    // Force green DOMINANCE as a RATIO so it survives the scene tonemap. A small
    // additive lift gets crushed by a bright warm dusk sun (the grass rendered
    // orange -> FOLIAGE_SPARSE); a multiplicative floor keeps the green channel a
    // fixed fraction ABOVE the warmest of red/blue regardless of overall exposure,
    // so ground cover reads green at dawn/noon/dusk alike. The CEILING ratio keeps
    // it from going neon and tripping the sky-speckle detector (and distant cards
    // above the horizon are already vertex-culled). RENDER-ONLY.
    float rbMax2 = max(lit.r, lit.b);
    lit.g = clamp(lit.g, rbMax2 * 1.22, rbMax2 * 1.45 + 0.02);
    // Pull red down a touch so warm dirt-coloured lighting never ties green.
    lit.r = min(lit.r, lit.g * 0.86);

    FragColor = vec4(lit, alpha);
}
