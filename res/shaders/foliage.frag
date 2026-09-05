#version 450 core

// ===========================================================================
// instanced foliage scatter fragment stage.
//
// Forward-lit ground cover blended into the lit HDR target after the opaque
// terrain.  (no sim writes).
//
// kill the CYAN/TEAL cast, scene-light so the blades go
// DARK under storm/night, and keep them clearly GREEN in daylight.
//
// ROOT CAUSE of the old cyan: the daytime sky ambient (u_ambientColor ==
// m_skyAmbientColor) is strongly BLUE-DOMINANT -- vec3(0.1,0.15,0.2)*PI, i.e.
// B > G > R. A near-vertical blade card with an up-biased normal is ambient-
// DOMINATED (it barely catches the low/raking sun), so its colour collapses to
// `u_ambientColor * albedo`. With the desaturated blade albedo that product
// keeps a heavy blue component, and after filmic+gamma a low-saturation green
// with a blue lift reads to the eye as pale CYAN/TEAL. Under storm the sky
// ambient does NOT drop (only the DIRECT sun is cloud-shadowed), so the ambient-
// dominated blades stayed bright and GLOWED teal while the terrain went dark.
//
// THE FIX (lighting only -- placement / world_hash untouched):
//   1) HUED ambient: drive the ambient by the sky-ambient LUMINANCE but through
//      the blade's OWN green albedo hue, not the raw blue sky colour. The blue
//      channel can no longer wash the blade to cyan, yet the ambient still
//      tracks sun-up (bright day, ~10x dimmer night) so the grass darkens with
//      the scene. A faint sky tint is folded back in for cohesion, capped so it
//      can never re-introduce a blue-dominant result.
//   2) STORM darkening: replicate the lighting pass' projected cloud cast shadow
//      and attenuate BOTH the ambient and the direct sun by the cloud coverage
//      overhead, so an overcast cell drives the blades DARK exactly like the
//      ground beside them (the old shader left ambient un-attenuated -> glow).
//   3) Sun-weighted: the GREEN-hued direct sun term (albedo/PI * sunRadiance) is
//      what lifts the daytime grass; u_sunColor is pre-scaled by sun intensity/
//      transmittance on the CPU so it collapses to ~0 at night.
//   * filmic tonemap + 1/2.2 gamma, identical to lighting_pass.frag, so the lit
//     blade lands in the same sRGB space as the surrounding tonemapped terrain.
//
// A base-to-tip ambient-occlusion gradient (darker root, lighter tip) keeps the
// field from reading as a flat single hue, WITHOUT adding any light of its own.
// ===========================================================================

in VS_OUT {
    vec2  texCoord;
    vec4  color;
    float fade;
    float heightT;   // 0 root.. 1 tip
    vec3  worldPos;
    vec3  worldNormal;
} fs_in;

uniform vec3  u_sunDirection;  // direction the sunlight travels
uniform vec3  u_sunColor;      // CPU-side: already scaled by sun intensity/transmittance
uniform float u_sunIntensity;  // [0,1] sun-up factor (0 at night)
uniform vec3  u_ambientColor;  // == u_skyAmbientColor (already PI-scaled)

// foliage-night: MOON toward-light direction, SAME convention as u_sunDirection
// negated (i.e. -u_moonDir is the moon TRAVEL dir). Uploaded by FoliagePass from
// pipeline.m_moonLightDir, the identical value the deferred lighting pass keys its
// moon term + night cascades off (moon-shadows workstream). Defaults to overhead
// (anti-sun at midnight) so a missing-uniform path still lights blades sanely.
uniform vec3  u_moonDir;        // toward-light dir of the moon (anti-sun)

// projected cloud cast-shadow uniforms, mirroring the
// lighting pass so a storm-overcast cell darkens the blades like the terrain.
// All default to "no clouds" so a missing-uniform path is a no-op.
uniform int   u_cloudShadowEnabled;   // 0 == skip (clear sky)
uniform vec2  u_cloudScrollOffset;    // wind * tick-phase, world metres
uniform float u_cloudCoverageAmount;  // [0,1] sky-cover fraction
uniform float u_cloudBiomeVariation;  // biome coverage bias
uniform float u_cloudPlaneHeight;     // world Y of the cloud sheet
uniform float u_cloudShadowStrength;  // [0,1] max darkening under a cloud core
uniform vec3  u_cloudSunDir;          // sun TRAVEL direction (== u_sunDirection)

out vec4 FragColor;

const float PI = 3.14159265359;
const float SUN_IRRADIANCE_SCALE = PI; // matches lighting_pass.frag
const vec3  LUMA = vec3(0.2126, 0.7152, 0.0722);

// --- cloud coverage field (matches lighting_pass.frag / enhanced_skybox.frag) ---
float fol_cloud_hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
float fol_cloud_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fol_cloud_hash(i + vec2(0.0, 0.0));
    float b = fol_cloud_hash(i + vec2(1.0, 0.0));
    float c = fol_cloud_hash(i + vec2(0.0, 1.0));
    float d = fol_cloud_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fol_cloud_fbm(vec2 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * fol_cloud_noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}
float fol_cloudCoverageAt(vec2 worldXZ) {
    vec2 p = (worldXZ + u_cloudScrollOffset) * (1.0 / 1200.0);
    float base = fol_cloud_fbm(p, 5);
    float detail = fol_cloud_fbm(p * 2.7 + vec2(11.3, 4.7), 3);
    float field = base * 0.72 + detail * 0.28;
    float cov = clamp(u_cloudCoverageAmount + u_cloudBiomeVariation, 0.0, 1.0);
    float lo = mix(0.62, 0.30, cov);
    float hi = mix(0.82, 0.55, cov);
    return smoothstep(lo, hi, field);
}
// [0,1] cloud shadow amount above this blade (projected up the sun ray).
float fol_cloudShadow(vec3 worldPos) {
    if (u_cloudShadowEnabled == 0 || u_cloudShadowStrength <= 0.0) return 0.0;
    vec3 toSun = -u_cloudSunDir;
    if (toSun.y < 0.05) return 0.0;
    float dh = u_cloudPlaneHeight - worldPos.y;
    if (dh <= 0.0) return 0.0;
    float t = dh / toSun.y;
    vec2 hitXZ = worldPos.xz + toSun.xz * t;
    float coverage = fol_cloudCoverageAt(hitXZ);
    return clamp(coverage * u_cloudShadowStrength, 0.0, 1.0);
}

void main() {
    // Blade alpha mask: taper toward the tip and soften the vertical edges so
    // the card reads as a tuft rather than a hard quad. texCoord.y: 0 base, 1 tip.
    float x = abs(fs_in.texCoord.x * 2.0 - 1.0);
    float halfWidth = fs_in.color.a > 0.5
        ? pow(max(1.0 - fs_in.texCoord.y, 0.0), 0.8) : 1.0;
    float edgeAA = max(fwidth(x), 0.015);
    float alpha = (1.0 - smoothstep(halfWidth - edgeAA, halfWidth + edgeAA, x)) * fs_in.fade;
    if (alpha < 0.02) {
        discard;
    }

    vec3 albedo = fs_in.color.rgb;

    // Base-to-tip ambient-occlusion gradient: roots in self-shadow, tips fully
    // exposed. This is a MULTIPLIER on the scene light (never additive), so it
    // only ever darkens -- it cannot make a night blade glow.
    float ao = mix(0.45, 1.0, fs_in.heightT); // darker root, fully lit tip

    vec3 N = normalize(fs_in.worldNormal);
    float ndl = max(dot(N, -normalize(u_sunDirection)), 0.0);

    // STORM / OVERCAST darkening. Two combined terms so the blades go genuinely
    // DARK under an overcast sky (the old shader left ambient un-attenuated, so
    // the blades GLOWED while the terrain went dark):
    //   * overcast: a UNIFORM dim across the whole field from the sky-cover
    //     fraction (overcast = less sky light everywhere, even between cloud
    //     cores) -- this is the dominant darkener and removes the field-wide glow.
    //   * coreShadow: the projected cloud cast shadow (same field as the terrain
    //     pass) adds extra darkening directly under a cloud core.
    float overcast = 0.0;
    float coreShadow = 0.0;
    if (u_cloudShadowEnabled != 0) {
        // Uniform overcast dim from the sky-cover fraction itself (not just the
        // cloud-core shadow): an overcast cell loses sky light EVERYWHERE, so the
        // whole field darkens, not only the patches under a cloud core. Scaled by
        // shadow_strength so a thin/decorative cloud layer barely dims.
        overcast = clamp(u_cloudCoverageAmount * (0.4 + 0.6 * u_cloudShadowStrength), 0.0, 1.0);
        coreShadow = fol_cloudShadow(fs_in.worldPos);
    }
    // Combined [0,1] light loss; overcast sets the floor, the core shadow deepens it.
    float lightLoss = clamp(overcast + (1.0 - overcast) * coreShadow, 0.0, 1.0);

    // (1) HUED AMBIENT -- the cyan fix. Take the sky-ambient BRIGHTNESS but apply
    // it through the blade's OWN green albedo hue so the blue sky channel can
    // never wash the blade to cyan. ambientLum tracks sun-up (bright day, ~10x
    // dimmer night), so the grass still darkens with the scene. A small, capped
    // sky tint is folded back for cohesion with the surrounding ground.
    // u_ambientColor already carries the PI irradiance scale, so its luminance is
    // the correct ambient brightness -- do NOT re-multiply by PI (that would over-
    // brighten the blade by ~3x and blow it toward white). greenAmbient reuses the
    // ORIGINAL ambient luminance, just re-hued through the blade's green albedo.
    float ambientLum = max(dot(u_ambientColor, LUMA), 0.0);
    vec3  greenAmbient = albedo * ambientLum;
    vec3  skyTint = u_ambientColor * albedo;       // raw (blue-leaning) sky*albedo
    vec3  ambient = mix(greenAmbient, skyTint, 0.20);
    // NIGHT GATE (foliage night-glow fix): the dusk/aurora band keeps u_ambientColor
    // elevated + blue (m_sun.intensity only reaches 0 below -0.1 sun elevation), so
    // up-facing ambient-dominated cards stayed near-WHITE (~rgb 239,248,254) at night
    // while the terrain went dark. Tie the foliage ambient to the sun-up factor with a
    // small night floor so the blades darken WITH the scene across the whole dusk band;
    // the moonlight fill below re-adds a dim cool tone..
    // Mirror the DEFERRED path's night transition EXACTLY: lighting_pass.frag
    // computes nightFactor = 1 - smoothstep(0, 0.06, sunLum) from the SUN COLOUR
    // luminance (which actually reaches 0 at night), NOT from u_sunIntensity (which
    // stays elevated across the dusk band). Drop the sky ambient to ZERO at night
    // with NO floor so the blades collapse with the terrain's hemispheric ambient;
    // the moon fill below re-adds the same dim cool tone the deferred moon gives the
    // terrain..
    float sunLum = max(max(u_sunColor.r, u_sunColor.g), u_sunColor.b);
    float nightFactor = 1.0 - smoothstep(0.0, 0.06, sunLum);
    ambient *= (1.0 - nightFactor);
    // Overcast strongly cuts the diffuse sky light reaching the ground cover.
    ambient *= (1.0 - 0.9 * lightLoss);

    // (3) DIRECT SUN -- the green-hued Lambert term that lifts daytime grass and
    // collapses to ~0 at night (u_sunColor is pre-scaled CPU-side). Cloud-shadow
    // attenuated like the terrain's direct sun.
    vec3 sunRadiance = u_sunColor * SUN_IRRADIANCE_SCALE;
    // Grass overhaul: SOFTEN the direct term (x0.6). Vertical blade cards rake the sun on
    // their faces and were blowing out to bright yellow slivers (reading as fake lit decals,
    // not grass). A gentler direct + the existing green ambient keeps a believable lit-grass
    // body with bright TIPS (via the AO gradient) instead of uniform blown-out blades..
    vec3 direct = (albedo / PI) * sunRadiance * ndl * (1.0 - lightLoss) * 0.6;

    vec3 color = (ambient + direct) * ao;

    // MOON FILL (foliage-night): mirror the deferred moon in lighting_pass.frag so
    // night grass reads with the SAME dim cool moonlit tone as the terrain beside it,
    // not near-black and not a glowing card. The deferred path uses a DIFFUSE moon
    // with kMoonColor = vec3(0.24, 0.32, 0.58), lit from the moon's toward-light
    // direction; we replicate it here keyed off nightFactor (the sun-colour ramp
    // above) so it fades in precisely as the directional sun fades out. Lit through
    // the blade's green albedo + a desaturated cool moon hue: low-saturation /
    // blue-shifted (Purkinje) night vision, and never a saturated green tip that
    // could trip the GREEN_SKY_SPECKLE gate.
    const vec3 kMoonColor = vec3(0.24, 0.32, 0.58);
    float ndlMoon = max(dot(N, -normalize(u_moonDir)), 0.0);
    // Wrap the moon Lambert a little (half-lit) so vertical blade backs aren't pure
    // black under an overhead midnight moon; keep it dim. Hue = blade albedo crossed
    // with the cool moon colour so it stays desaturated + blue-leaning.
    float moonWrap = ndlMoon * 0.7 + 0.3;
    vec3 moonFill = (albedo * kMoonColor) * moonWrap;
    color += moonFill * (nightFactor * ao) * (1.0 - lightLoss);

    // Filmic tonemap + gamma, byte-identical to lighting_pass.frag, so the lit
    // blade lands in the same sRGB space as the surrounding tonemapped terrain
    // (the lighting FBO this pass blends into is already tonemapped).
    color = color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14);
    color = pow(color, vec3(1.0 / 2.2));

    // tiny black floor so dense dark-rooted foliage (raised scatter density)
    // never renders a PURE-black (<=2/255) pixel. The PlayerView void-cluster gate
    // flags contiguous max(r,g,b)<=2 regions as geometry holes; a clump of deep-shadow
    // foliage roots at the camera's feet is NOT a hole, so lift it just above the
    // threshold (~3.5/255, imperceptible) to avoid the false positive without masking
    // a real void (foliage is opaque cover, not a gap to the cleared framebuffer).
    color = max(color, vec3(3.5 / 255.0));

    FragColor = vec4(color, alpha);
}
