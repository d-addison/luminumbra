#version 450 core

// ===========================================================================
// T-I5b-1 (F1): instanced foliage scatter fragment stage.
//
// Forward-lit ground cover blended into the lit HDR target after the opaque
// terrain. RENDER-ONLY (no sim writes).
//
// T-I5b-DR-foliage-emissive: foliage is now SCENE-LIT exactly like the terrain
// ground, with NO self-emissive term whatsoever. The previous fix added an
// additive "green self-glow floor" plus a multiplicative green-dominance clamp
// that forced the blades to stay bright green regardless of the sun -- so the
// grass GLOWED IN THE DARK (montage_summer_night). Both are removed.
//
// The lighting here mirrors lighting_pass.frag's diffuse model so foliage sits
// in the SAME colour space as the terrain it overlays:
//   * ambient  = u_ambientColor * albedo            (u_ambientColor already
//                carries the PI irradiance scale, matching u_skyAmbientColor)
//   * direct   = albedo/PI * (u_sunColor * PI) * NdotL   (Lambert, same
//                SUN_IRRADIANCE_SCALE = PI as the terrain pass; u_sunColor is
//                already pre-scaled by sun intensity / transmittance on the CPU,
//                so it collapses to ~0 at night)
//   * filmic tonemap + 1/2.2 gamma                  (identical to the terrain
//                pass) so the lit blade matches the surrounding ground exactly.
// Result: bright in daylight, DARK at night, never self-emissive.
//
// A base-to-tip ambient-occlusion gradient (darker root, lighter tip) and the
// per-instance tonal/hue jitter baked into the vertex colour keep the field
// from reading as a flat single hue, WITHOUT adding any light of its own.
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
uniform vec3  u_sunColor;      // CPU-side: already scaled by sun intensity/transmittance
uniform float u_sunIntensity;  // [0,1] sun-up factor (0 at night)
uniform vec3  u_ambientColor;  // == u_skyAmbientColor (already PI-scaled)

out vec4 FragColor;

const float PI = 3.14159265359;
const float SUN_IRRADIANCE_SCALE = PI; // matches lighting_pass.frag

void main() {
    // Blade alpha mask: taper toward the tip and soften the vertical edges so
    // the card reads as a tuft rather than a hard quad. texCoord.y: 0 base, 1 tip.
    float edge = 1.0 - abs(fs_in.texCoord.x * 2.0 - 1.0); // 0 at sides, 1 centre
    float taper = mix(1.0, 0.25, fs_in.texCoord.y);       // narrower at the tip
    float alpha = smoothstep(0.0, 0.35, edge * taper) * fs_in.fade;
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

    // SCENE LIGHTING, identical model to the terrain ground:
    //   ambient term (sky irradiance) + Lambert sun diffuse.
    // u_ambientColor and u_sunColor both already carry their irradiance scale on
    // the CPU; u_sunColor additionally folds in the sun-up intensity so it is ~0
    // at night. NO constant/self-emissive floor.
    vec3 ambient = u_ambientColor * albedo;
    vec3 sunRadiance = u_sunColor * SUN_IRRADIANCE_SCALE;
    vec3 direct = (albedo / PI) * sunRadiance * ndl;

    vec3 color = (ambient + direct) * ao;

    // Filmic tonemap + gamma, byte-identical to lighting_pass.frag, so the lit
    // blade lands in the same sRGB space as the surrounding tonemapped terrain
    // (the lighting FBO this pass blends into is already tonemapped).
    color = color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14);
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, alpha);
}
