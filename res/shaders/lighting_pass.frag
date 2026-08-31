#version 450 core
#include "config_constants.gen.glsl"
out vec4 FragColor;

in vec2 TexCoords;

// G-Buffer samplers (match g_buffer.frag output names)
uniform sampler2D gPosition;          // RGB16F: full view-space position
uniform sampler2D gNormalMaterial;    // RGB10A2: Octahedral normal + material ID
uniform sampler2D gAlbedoRoughness;   // RGBA8: RGB albedo + roughness
uniform sampler2D gMetallicAO;        // RG16F: Metallic + AO
uniform sampler2D u_ssao;

// Material lookup (256 x 4 rows; row 2 holds emissive_intensity/scale)
uniform sampler2D u_materialLUT;
// derive row-center v-coords from a single row count (must match
// RenderPipeline::init_material_lut ROWS and g_buffer.frag LUT_ROWS).
const int LUT_ROWS = 4;
float lutRowV(int row) { return (float(row) + 0.5) / float(LUT_ROWS); }
// Rescales the normalized emissive_intensity LUT column back to world units.
// Must match RenderPipeline::kEmissiveLutScale.
uniform float u_emissiveLutScale = 8.0;

// Aetheric scalar field emissive tap. The field (R32F, extent x extent
// cells) is sampled at the fragment's world XZ and adds an additive emissive
// glow. GATED by u_aetherActive (0 when no field is uploaded -> zero contribution
// -> pixel-identical, so shipped render paths stay RenderHealth-neutral until
// game content drives sparse aether sources). Engine knows only "emissive scalar
// field"; the glow color/intensity is an engine default game content can refine.
uniform sampler2D u_aetherField;
uniform vec2 u_aetherFieldWorldOrigin;   // world XZ of the grid's (0,0) corner
uniform float u_aetherFieldInvWorldSpan; // 1 / (extent * cell_size_m)
uniform float u_aetherActive = 0.0;      // 0 = no field uploaded (no glow)
uniform vec3 u_aetherGlowColor = vec3(0.30, 0.55, 0.95);
uniform float u_aetherGlowIntensity = 2.0;
//  ( -6): emissive-MATERIAL modulation by the local
// aether — crystalGlow scales by (1 + aether * modulation). Default 0.0 is a
// multiply by exactly 1.0, so the untouched path is pixel-identical even with
// an active field tap; luminance is monotonic non-decreasing in the uniform.
uniform float u_aetherMaterialModulation = 0.0;
//  ( -8): Lumin/Umbra polarity tint. When the RG32F
// dual tap is live (u_aetherPolarityActive = 1), the field's G channel in
// [-1, 1] mixes the glow color toward the matching pole by |polarity|.
// Default 0.0 (single-channel/no upload) leaves the glow color untouched —
// pixel-identical by construction.
uniform float u_aetherPolarityActive = 0.0;
uniform vec3 u_aetherPolarityColorPos = vec3(0.95, 0.85, 0.55); // Lumin pole
uniform vec3 u_aetherPolarityColorNeg = vec3(0.45, 0.30, 0.85); // Umbra pole
//  ( snow cover): render-only snow ground cover [0,1]. 0.0 (the default)
// is byte-identical; >0 blends UP-FACING surfaces toward snow white + full rough.
uniform float u_snowCover = 0.0;

// G-Buffer decoding functions
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}

vec3 decode_octahedral(vec2 encoded) {
    vec2 e = encoded * 2.0 - 1.0;
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0) v.xy = octWrap(v.xy);
    return normalize(v);
}

// Shadow and other uniforms
uniform sampler2DArray u_shadowCascades;
uniform mat4 u_lightSpaceMatrices[4];
uniform vec4 u_cascadeSplits;
// the tinted-transmission cascade. WHITE where no glass
// occludes the key light, the accumulated Beer-Lambert product where it does
// (shadow_tint.frag). u_shadowTintEnabled==0 skips the sampling entirely - the
// same-build A/B lever (off vs on-with-white must FLIP to exactly 0.0).
uniform sampler2DArray u_shadowTintCascades;
uniform int u_shadowTintEnabled;

// Texture and world uniforms
uniform sampler2DArray u_terrainTextures;
uniform sampler2D u_causticsTexture;
uniform float u_time;
uniform float u_sea_level;

uniform vec3 u_terrainOrigin;
uniform vec3 u_viewPos;
uniform mat4 u_inverseView;
uniform vec3 u_skyAmbientColor;
struct SunLight {
    vec3 direction;
    vec3 color;
};
uniform SunLight u_sun;
// moon-shadows: the moon's toward-light direction (anti-sun, overhead at
// midnight), uploaded by LightingPass.cpp from m_moonLightDir. Used directly as
// the L vector for the night moon term AND to key the cast-shadow lookup, since
// at night get_light_space_matrices builds the cascade from this same direction.
uniform vec3 u_moonDir;
//  rendering: lunar illumination [0,1] -> the "two night modes". 1 = full
// moon (bright, navigable night + crisp moon shadows); ~0 = new moon (dark, wants a torch).
uniform float u_moonIllum = 1.0;
// The moon wrap floor remains a supported photo-mode grading control. The
// shipped 0.25 default keeps moonlit nights navigable; 0.0 selects a moodier,
// floorless midnight without requiring a different shader build.
uniform float u_moonWrapFloor = 0.25;
//  rendering (, ): the moon's OWN radiance channel — the cool key
// colour, set from C++ (RenderContext.moon_radiance) instead of a hardcoded shader const,
// so the moon can be calibrated/tuned independently of the sun. Default == the prior
// hardcoded kMoonColor so an unset value is byte-identical.
uniform vec3 u_moonRadiance = LUMIN_MOONLIGHT_COLOR;

#define MAX_POINT_LIGHTS 32
struct PointLight {
    vec3 position;
    float radius;
    vec3 color;
    float intensity;
};
uniform PointLight u_pointLights[MAX_POINT_LIGHTS];
uniform int u_pointLightCount;

// --- Cave / sky-visibility ambient occlusion (render-only, default OFF) ---
// SSAO uses a ~0.8m radius, so a fragment deep in a cave still receives the FULL
// daylight sky-ambient and reads as flat-lit grey, swamping any point light placed
// there. This term marches a short ray from the fragment toward the sky through the
// view-space G-buffer; if geometry blocks the upward ray the fragment is occluded
// from the sky and its AMBIENT (sky+ground-bounce) fades toward u_caveAmbientFloor.
// Sun/moon/point lights are NOT touched, so a lamp in a now-dark cave reads naturally.
// GATED by u_caveAmbientOcclusion (0 = OFF => skyVis==1 => pixel-identical to pre-fix).
uniform float u_caveAmbientOcclusion;  // master gate: 0 = OFF (default, C++ sets it)
uniform float u_caveSkyMaxDist;        // metres of upward ray to probe
uniform int   u_caveSkySteps;          // march samples
uniform float u_caveAmbientFloor;      // residual ambient deep inside (0..1)
uniform float u_caveThickness;         // m: occluder depth band
uniform mat4  u_projection;            // view -> clip, to step the ray in screen space
uniform vec2  u_screenSize;            // pixels

// wind-advected cloud cast-shadow uniforms. The coverage field is a
// pure function of replicated weather state + tick + wind, evaluated render-side;
// nothing here writes back into the sim (regression contract, one-way). directSun is
// scaled by (1 - cloudShadow(worldPos)) so cloud cores throw crawling terrain
// shadows that drift with the SAME wind scroll as the sky-dome clouds. The fbm/
// hash/cloudCoverageAt trio below is byte-identical to enhanced_skybox.frag so the
// dome cloud and its ground shadow stay registered (GLSL has no shared includes).
//   u_cloudShadowEnabled  - 0 disables the sample entirely (zero added cost)
//   u_cloudScrollOffset   - wind * tick-phase, world metres (matches the dome)
//   u_cloudCoverageAmount - [0,1] weather sky-cover fraction
//   u_cloudBiomeVariation - biome coverage bias
//   u_cloudPlaneHeight    - world Y of the cloud sheet
//   u_cloudShadowStrength - [0,1] max darkening the cloud sheet applies to the sun
//   u_cloudSunDir         - sun TRAVEL direction (same as u_sun.direction)
uniform int   u_cloudShadowEnabled = 0;
uniform vec2  u_cloudScrollOffset = vec2(0.0);
uniform float u_cloudCoverageAmount = 0.45;
uniform float u_cloudBiomeVariation = 0.0;
uniform float u_cloudPlaneHeight = 900.0;
uniform float u_cloudShadowStrength = 0.0;
uniform vec3  u_cloudSunDir = vec3(0.0, -1.0, 0.0);

const float PI = 3.14159265359;

// ---  cloud coverage field (MUST match enhanced_skybox.frag verbatim) ---
float cloud_hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
float cloud_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f*f*(3.0-2.0*f);
    float a = cloud_hash(i + vec2(0.,0.));
    float b = cloud_hash(i + vec2(1.,0.));
    float c = cloud_hash(i + vec2(0.,1.));
    float d = cloud_hash(i + vec2(1.,1.));
    return mix(mix(a,b,f.x), mix(c,d,f.x), f.y);
}
float cloud_fbm(vec2 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for(int i = 0; i < octaves; i++) {
        value += amplitude * cloud_noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}
float cloudCoverageAt(vec2 worldXZ) {
    // larger cloud clusters (~2400 m feature scale, was 1200) for bolder,
    // more pronounced cloud masses + their cast shadows. MUST stay identical to
    // enhanced_skybox.frag::cloudCoverageAt so the dome cloud and its ground
    // shadow remain registered.
    vec2 p = (worldXZ + u_cloudScrollOffset) * (1.0 / 2400.0);
    float base = cloud_fbm(p, 5);
    float detail = cloud_fbm(p * 2.7 + vec2(11.3, 4.7), 3);
    float field = base * 0.72 + detail * 0.28;
    float cov = clamp(u_cloudCoverageAmount + u_cloudBiomeVariation, 0.0, 1.0);
    float lo = mix(0.62, 0.30, cov);
    float hi = mix(0.82, 0.55, cov);
    return smoothstep(lo, hi, field);
}

// Projected cloud cast shadow at a world position: walk from the fragment up the
// sun ray to the cloud plane, sample the coverage at that XZ, return the [0,1]
// shadow amount. The projection along the sun direction is what makes the shadow
// CRAWL across terrain as the sun moves / the clouds drift -- a real cast shadow,
// not a screen-space overlay.
float cloudShadow(vec3 worldPos) {
    if (u_cloudShadowEnabled == 0 || u_cloudShadowStrength <= 0.0) return 0.0;
    // toward-sun = -travel direction. Skip when the sun is at/below the horizon.
    vec3 toSun = -u_cloudSunDir;
    if (toSun.y < 0.05) return 0.0;
    float dh = u_cloudPlaneHeight - worldPos.y;
    if (dh <= 0.0) return 0.0;
    float t = dh / toSun.y;                 // distance along the sun ray to the plane
    vec2 hitXZ = worldPos.xz + toSun.xz * t;
    // Soft PENUMBRA: average the coverage over a small kernel at the cloud plane so
    // the cast shadow has soft edges instead of a hard decal. Only paid when cloud
    // shadows are enabled (early-out above), so the common in-game path is free.
    const float r = 70.0; // metres at the cloud plane
    float coverage = cloudCoverageAt(hitXZ) * 0.40
                   + cloudCoverageAt(hitXZ + vec2( r, 0.0)) * 0.15
                   + cloudCoverageAt(hitXZ + vec2(-r, 0.0)) * 0.15
                   + cloudCoverageAt(hitXZ + vec2(0.0,  r)) * 0.15
                   + cloudCoverageAt(hitXZ + vec2(0.0, -r)) * 0.15;
    return clamp(coverage * u_cloudShadowStrength, 0.0, 1.0);
}

// ---: exposure / irradiance transfer ---
// The diffuse BRDF below divides albedo by PI (kD * albedo / PI), the
// energy-conserving Lambert term. For that to render an albedo faithfully, the
// incoming SUN radiance must be the surface IRRADIANCE, i.e. ~PI for a unit-
// color overhead sun: outgoing = albedo/PI * irradiance -> albedo when
// irradiance = PI. The pipeline authors u_sun.color as a unit-ish sun COLOR
// (noon ~ (1.0, 0.95, 0.85)); feeding it directly left every lit surface dark
// by a factor of ~PI (the unmatched 1/PI division ate the luminance). An 18%
// gray surface landed at on-screen sRGB ~0.32 and sand/grass read as
// rust-brown / near-black at noon. Multiplying the sun radiance by PI converts
// the authored sun color into physical irradiance at the root, so a white
// surface tends to white (filmic-rolled) and an 18% gray lands near perceptual
// mid (~0.58). Sun ONLY: point lights already carry explicit intensity, and
// ambient is an irradiance term already. See the exposure audit in the
//  commit for the full before/after transfer table.
const float SUN_IRRADIANCE_SCALE = PI;

//  realistic-lighting grade (controllable). The flat sky-ambient fill plus
// the desaturating filmic tonemap leave noon terrain washed and low-contrast.
// These grade controls restore punch: exposure lifts midtones before the curve;
// saturation/contrast/warmth are applied after it. All default to identity so an
// unset pipeline reproduces the prior look exactly. Driven from the pipeline
// (LightingPass) and tunable via the LUMIN_GRADE env override.
uniform float u_exposure = 1.0;
uniform float u_saturation = 1.0;
uniform float u_contrast = 1.0;
uniform vec3 u_lightWarmth = vec3(1.0, 1.0, 1.0);
//  cinematic split-tone (warm palette-style key/fill): tint shadows toward
// u_shadowTint (cool) and highlights toward u_highlightTint (warm), blended by
// luma, scaled by u_splitToneStrength. Defaults identity (no tint).
uniform vec3 u_shadowTint = vec3(1.0, 1.0, 1.0);
uniform vec3 u_highlightTint = vec3(1.0, 1.0, 1.0);
uniform float u_splitToneStrength = 0.0;

// Optimized PBR functions with precalculated values
float DistributionGGX(float NdotH, float a2) {
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float k) {
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
// terrain PBR: roughness-aware Fresnel for the ambient/environment term. Rougher
// surfaces reflect less of the environment at grazing angles (the reflection
// blurs out), so the grazing limit is clamped toward (1 - roughness) instead of
// 1. Used ONLY for the analytic ambient specular below — the direct-light path
// keeps the sharp fresnelSchlick. (Karis 2013, "Real Shading in UE4".)
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    vec3 Fr = max(vec3(1.0 - roughness), F0);
    return F0 + (Fr - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
float CalculateShadow(vec3 fragPos, vec3 normal, vec3 lightDir, float viewDepth) {
    // 1. Determine which cascade to use in a branchless way
    int cascadeIndex = 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.x) ? 1: 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.y) ? 1: 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.z) ? 1: 0;

    // 2. Project fragment into the chosen light space
    vec4 fragPosLightSpace = u_lightSpaceMatrices[cascadeIndex] * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if(projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0) {
        // Outside the last cascade's light frustum: no shadow information
        // exists, so treat the fragment as LIT (standard CSM out-of-range
        // convention). Returning 0.0 here rendered everything beyond the
        // shadow range sun-unlit - invisible before the far-LOD horizon
        // existed, pitch-black mountains after it.
        return 1.0;
    }

    // 3. PCF (Percentage-Closer Filtering)
    float currentDepth = projCoords.z;
    float shadow = 0.0;
    float bias = max(0.005 * (1.0 - dot(normal, lightDir)), 0.001);
    vec2 texelSize = 1.0 / vec2(textureSize(u_shadowCascades, 0).xy);

    // 5x5 PCF (25 taps) for softer shadow edges / less harsh terminators than the
    // prior 3x3. Cheap in this pass (lighting is ~0.15 ms); soft contact shadows
    // read far more naturally than the hard 3x3 step.
    for(int x = -2; x <= 2; ++x) {
        for(int y = -2; y <= 2; ++y) {
            float pcfDepth = texture(u_shadowCascades, vec3(projCoords.xy + vec2(x, y) * texelSize, cascadeIndex)).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0: 0.0;
        }
    }
    return 1.0 - (shadow / 25.0);
}
// sample the tinted transmission at the SAME cascade
// projection CalculateShadow uses. Out-of-range = white (no tint information),
// matching CSM's lit convention. The standard colored-shadow-map approximation:
// the tint applies to every receiver at the texel (a receiver BETWEEN the light
// and the glass is also tinted; rare in practice and covered by the visual contract.
vec3 SampleShadowTint(vec3 fragPos, float viewDepth) {
    if (u_shadowTintEnabled == 0) {
        return vec3(1.0);
    }
    int cascadeIndex = 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.x) ? 1: 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.y) ? 1: 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.z) ? 1: 0;
    vec4 fragPosLightSpace = u_lightSpaceMatrices[cascadeIndex] * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) {
        return vec3(1.0);
    }
    return texture(u_shadowTintCascades, vec3(projCoords.xy, cascadeIndex)).rgb;
}
vec3 TriPlanar(vec3 worldPos, vec3 normal, sampler2DArray texArray, float layer, float scale) {
    vec3 relPos = worldPos - u_terrainOrigin; vec2 uv_y=relPos.xz*scale; vec2 uv_x=relPos.zy*scale; vec2 uv_z=relPos.xy*scale;
    vec3 tex_y=texture(texArray,vec3(uv_y,layer)).rgb; vec3 tex_x=texture(texArray,vec3(uv_x,layer)).rgb; vec3 tex_z=texture(texArray,vec3(uv_z,layer)).rgb;
    vec3 weights=pow(abs(normal),vec3(3.0)); weights=normalize(weights);
    return tex_y*weights.y+tex_x*weights.x+tex_z*weights.z;
}

vec3 CalculateLightContribution(vec3 L, vec3 V, vec3 N, vec3 F0, vec3 albedo, float metallic, float a2, float k, vec3 radiance) {
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Optimized PBR calculations with precalculated values
    float NDF = DistributionGGX(NdotH, a2);
    float G   = GeometrySmith(NdotV, NdotL, k);
    vec3  F   = fresnelSchlick(HdotV, F0);

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    // Optimized specular calculation
    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 0.0001);

    return (kD * albedo / PI + specular) * radiance * NdotL;
}

// Long-range sky visibility via an upward view-space ray-march against the
// G-buffer. Returns 1.0 = fully open to the sky, -> u_caveAmbientFloor when the
// fragment is roofed over (cave/overhang). Cheap: u_caveSkySteps depth taps.
// Only called when u_caveAmbientOcclusion > 0.5 (see main); zero cost when OFF.
float computeSkyVisibility(vec3 viewPosFrag, vec3 viewNrm) {
    // Sky direction in VIEW space (world +Y rotated by the view rotation). The
    // upper-left 3x3 of u_inverseView maps view->world; its transpose maps the
    // world up vector into view space.
    vec3 worldUp = vec3(0.0, 1.0, 0.0);
    vec3 skyDirView = normalize(transpose(mat3(u_inverseView)) * worldUp);
    // Start a bit off the surface along the normal to avoid self-occlusion, then
    // march toward the sky. Sample the G-buffer view-space position; if a sampled
    // surface is CLOSER to the eye than the ray point by more than u_caveThickness,
    // the ray is blocked.
    vec3 ro = viewPosFrag + viewNrm * 0.05;
    float occluded = 0.0;
    float stepLen = u_caveSkyMaxDist / float(max(u_caveSkySteps, 1));
    for (int s = 1; s <= 32; ++s) {
        if (s > u_caveSkySteps) break;          // dynamic bound (const loop cap)
        vec3 p = ro + skyDirView * (stepLen * float(s));
        // Project to screen.
        vec4 clip = u_projection * vec4(p, 1.0);
        if (clip.w <= 0.0) break;               // behind eye -> stop
        vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break; // off-screen = treat as open
        vec3 sampPos = texture(gPosition, uv).rgb;
        // gPosition is 0 where nothing was rendered (sky); skip those (open).
        if (dot(sampPos, sampPos) < 1e-6) continue;
        // The ray point p and the stored surface sampPos share the same screen
        // pixel. If the stored surface is in FRONT of the ray sample (closer to
        // the eye) within the thickness band, a ceiling/overhang occludes this
        // upward sample.
        float rayDepth  = length(p);
        float surfDepth = length(sampPos);
        if (surfDepth < rayDepth - 0.05 && (rayDepth - surfDepth) < u_caveSkyMaxDist) {
            // Weight nearer occluders more (closer ceiling = darker).
            occluded = max(occluded, 1.0 - float(s - 1) / float(u_caveSkySteps));
        }
    }
    // occluded in [0,1]; map to a sky-visibility multiplier in [floor,1].
    return mix(1.0, u_caveAmbientFloor, clamp(occluded, 0.0, 1.0));
}

void main() {
    // --- Step 1: Decode compressed G-Buffer ---

    vec3 viewPos = texture(gPosition, TexCoords).rgb;

    // Decode octahedral normal and material ID
    vec4 normalData = texture(gNormalMaterial, TexCoords);
    vec3 viewNormal = decode_octahedral(normalData.xy);
    uint MaterialID = uint(round(normalData.a * 255.0));

    // Get albedo and roughness
    vec4 albedoData = texture(gAlbedoRoughness, TexCoords);
    vec3 Albedo = albedoData.rgb;
    float Roughness = clamp(albedoData.a, 0.05, 1.0);

    // Get metallic and AO
    vec2 matData = texture(gMetallicAO, TexCoords).xy;
    float Metallic = matData.r;
    float ao = texture(u_ssao, TexCoords).r;

    // Transform to world space
    vec3 FragPos = vec3(u_inverseView * vec4(viewPos, 1.0));
    vec3 Normal = normalize(mat3(u_inverseView) * viewNormal);

    // triplanar terrain albedo + normal mapping now happen in
    // g_buffer.frag (the textured albedo and normal-mapped normal are baked into
    // the G-buffer), so the lighting pass consumes the G-buffer albedo directly.
    // The legacy lighting-pass TriPlanar override has been removed; u_terrainTextures
    // is retained as a binding for compatibility but no longer sampled here.

    //  (snow cover): SNOW COVER — blend UP-FACING surfaces toward snow (bright
    // albedo, fully rough) by the render-only cover scalar, BEFORE F0/roughness
    // derive from them. u_snowCover == 0 (the default) touches nothing:
    // byte-identical. The far-water sheet (200) keeps its authored look.
    if (u_snowCover > 0.001 && MaterialID != 200u) {
        float snowAmt = u_snowCover * clamp(Normal.y, 0.0, 1.0);
        Albedo = mix(Albedo, vec3(0.88, 0.91, 0.96), snowAmt);
        Roughness = mix(Roughness, 0.95, snowAmt);
    }

    vec3 V = normalize(u_viewPos - FragPos);
    vec3 F0 = mix(vec3(0.04), Albedo, Metallic);
    // the far-water sheet (material 200) is a flat
    // albedo-only sky-reflection-tint approximation. With ANY F0, grazing-angle
    // Fresnel turns the flat sheet into a sun-colored mirror at the eye-level
    // horizon views (the 1/NdotV specular term saturates white and swamps the
    // blue diffuse). Zero F0 makes it pure diffuse: the authored albedo IS the
    // look, matching the g_buffer case-200 design comment.
    if (MaterialID == 200u) {
        F0 = vec3(0.0);
    }

    // Precalculate roughness-dependent values once
    float a = Roughness * Roughness;
    float a2 = a * a;
    float r = Roughness + 1.0;
    float k = (r * r) / 8.0;

    // --- LIGHTING CALCULATION ---
    vec3 Lo = vec3(0.0);
    vec3 L_sun = normalize(u_sun.direction);
    float shadow = CalculateShadow(FragPos, Normal, L_sun, abs(viewPos.z));
    // Convert the authored sun COLOR into surface IRRADIANCE (x PI) so the
    // diffuse 1/PI division round-trips albedo faithfully (exposure-audit root
    // fix; see SUN_IRRADIANCE_SCALE above).
    vec3 sunRadiance = u_sun.color * SUN_IRRADIANCE_SCALE;
    // real cloud cast shadow. Project the fragment up the sun ray to
    // the cloud sheet, sample the wind-advected coverage, and attenuate the DIRECT
    // sun: directSun *= (1 - cloudShadow(worldPos)). Cloud cores throw crawling
    // terrain shadows that drift with the wind (the CloudShadow gate asserts the
    // moving-shadow luminance delta in a fixed terrain ROI). Render-only.
    float cloud_shadow = cloudShadow(FragPos);
    // tint the DIRECT key light by the glass
    // transmission along the light ray (white = no glass = identity). Applies to
    // whichever key the cascades hold (sun by day, the moon key at night).
    vec3 shadowTint = SampleShadowTint(FragPos, abs(viewPos.z));
    Lo += CalculateLightContribution(L_sun, V, Normal, F0, Albedo, Metallic, a2, k, sunRadiance)
          * shadow * (1.0 - cloud_shadow) * shadowTint;

    //  moonlight (owner: "there should be some brightness from the moon"):
    // a dim cool directional fill so night terrain reads as FORMED (directional
    // N.L gives contrast/detail) rather than a flat near-black sheet. Derived
    // entirely from the (working) sun uniforms: the moon is the anti-sun, and it
    // ramps in as the sun color fades to ~0 below the horizon (see L_moon below
    // for the direction). Diffuse-only + unshadowed (moonlight is soft; we
    // avoid a second shadow pass). u_sun.color is already x-transmittance scaled.
    float sunLum = max(u_sun.color.r, max(u_sun.color.g, u_sun.color.b));
    float nightFactor = 1.0 - smoothstep(0.0, 0.06, sunLum); // ~0 day -> ~1 night
    if (nightFactor > 0.001) {
        // The lighting pass uses u_sun.direction directly as the toward-light
        // vector L. The moon's TRAVEL direction is -u_sun.direction (anti-sun),
        // so its toward-light vector is +u_sun.direction — i.e. the same form the
        // sun uses, which correctly lights the up-facing terrain at night.
        // moon-shadows: the moon is now a REAL directional key that CASTS shadows,
        // not just a flat fill. L_moon is the uploaded toward-light dir (anti-sun,
        // overhead at midnight) — at night get_light_space_matrices builds the
        // shadow cascade from this SAME direction, so CalculateShadow(...,L_moon)
        // gives genuine moon cast/received shadows. Render-only.
        vec3 L_moon = normalize(u_moonDir);
        float NdotL_moon = max(dot(Normal, L_moon), 0.0);
        // Cast-shadow term from the (now moon-keyed) cascade. Moonlight is dim, so
        // a touch of softening: lift the floor a hair to avoid harsh black edges /
        // peter-panning from the wider-spread night cascade, while still reading as
        // a directional shadow.
        float moonShadow = CalculateShadow(FragPos, Normal, L_moon, abs(viewPos.z));
        //  rendering: the moon-shadow FLOOR scales with the lunar phase.
        // A FULL moon throws PROPER, crisp cast shadows (floor ~0.25 -> shadows read as real
        // directional contrast, DayZ bright-night feel); toward NEW moon the moon is too dim
        // to cast hard shadow, so the floor lifts (~0.7) and shadows fade out gracefully
        // rather than crushing the already-dark scene to black. (Was a flat 0.5.)
        float moonShadowFloor = mix(0.70, 0.25, u_moonIllum);
        moonShadow = mix(moonShadowFloor, 1.0, moonShadow);
        // Cool moonlight key. Brighter than the old fill so a moonlit night is
        // clearly NAVIGABLE (form + value), but obviously cooler + dimmer than day.
        // Terrain albedos are dark (linear ~0.01-0.07), so the albedo*radiance
        // product needs a strong cool key to lift night ground to a moonlit tone.
        //  rendering: scaled by u_moonIllum (lunar phase) -> a full moon is a
        // proper light source, a new moon barely lights the ground (the two night modes).
        // Moon key COLOUR now comes from u_moonRadiance (its dedicated C++ channel, ).
        const float kMoonKeyScale = 1.5; // overall full-moon brightness lever (DayZ-style navigable night)
        float moonKey = nightFactor * kMoonKeyScale * u_moonIllum * SUN_IRRADIANCE_SCALE;
        vec3 moonRadiance = u_moonRadiance * moonKey;
        // WRAPPED Lambert: an overhead midnight moon gives camera-facing SLOPES
        // NdotL~0, which left them pure black (the night ambient sits on the wrong
        // hemisphere to fill them). A modest wrap (NdotL*0.6+0.25) lets the moon
        // softly fill slopes + undersides so the night is NAVIGABLE, while the low
        // floor + cast-shadow term keep it clearly NIGHT (dim, directional) not a
        // flat day-bright wash.
        float moonWrap = NdotL_moon * 0.6 + u_moonWrapFloor;
        vec3 moonDiffuse = (Albedo / PI) * moonRadiance * moonWrap;
        // Soft, cheap specular highlight so wet/low-roughness night surfaces catch
        // a cool moon glint (sun uses the full BRDF; the moon gets a light Blinn-
        // Phong lobe to stay inexpensive). Reuses the existing N, V, and Roughness.
        vec3 H_moon = normalize(L_moon + V);
        float specPow = mix(8.0, 64.0, 1.0 - Roughness);
        float moonSpec = pow(max(dot(Normal, H_moon), 0.0), specPow) * (1.0 - Roughness) * 0.35;
        vec3 moonSpecular = u_moonRadiance * moonKey * moonSpec * NdotL_moon;
        vec3 moonLit = (moonDiffuse + moonSpecular) * moonShadow;
        // Desaturate toward the cool moon hue (Purkinje shift): warm (dusty) albedo
        // would otherwise read daytime-yellow under the key. Pull the lit result
        // partway toward its own luma scaled by the cool moon tint.
        //  rendering: the tint is a DESATURATED moon-grey, not a
        // strongly saturated blue. u_moonRadiance is already cool-biased, so the old
        // (0.55,0.75,1.35) target double-pulled foliage/bark to a garish electric
        // blue-purple. A gentle cool grey (0.70,0.80,1.05) keeps the Purkinje feel
        // (cool, slightly blue) while letting bark/foliage read as natural night tones.
        float moonLuma = dot(moonLit, vec3(0.2126, 0.7152, 0.0722));
        moonLit = mix(moonLit, moonLuma * vec3(0.70, 0.80, 1.05), 0.55);
        Lo += moonLit;
    }

    // Point Lights with early rejection
    for (int i = 0; i < u_pointLightCount; ++i) {
        vec3 lightVec = u_pointLights[i].position - FragPos;
        float dist = length(lightVec);

        // Early rejection for lights out of range
        if (dist < u_pointLights[i].radius) {
            float attenuation = 1.0 - smoothstep(0.0, u_pointLights[i].radius, dist);
            vec3 L_point = lightVec / dist; // Avoid normalize when we already have distance
            vec3 radiance = u_pointLights[i].color * u_pointLights[i].intensity * attenuation;
            Lo += CalculateLightContribution(L_point, V, Normal, F0, Albedo, Metallic, a2, k, radiance);
        }
    }

    // --- NEW: CAUSTICS CALCULATION ---
    vec3 caustics = vec3(0.0);
    // Apply caustics only if fragment is below sea level and on an upward-facing surface
    if (FragPos.y < u_sea_level && Normal.y > 0.5) {
        vec2 uv1 = FragPos.xz * 0.2 + vec2(u_time * 0.01, u_time * 0.015);
        vec2 uv2 = FragPos.xz * 0.15 - vec2(u_time * 0.02, u_time * 0.01);

        float caustic1 = texture(u_causticsTexture, uv1).r;
        float caustic2 = texture(u_causticsTexture, uv2).r;

        // Sum and modulate by sun intensity and shadow
        caustics = (caustic1 + caustic2) * u_sun.color * shadow * 0.5;
    }

    // --- MAGICAL CRYSTAL EFFECTS ---
    vec3 crystalGlow = vec3(0.0);
    // Fix: MaterialID is already decoded from normalData.a above, not matData.a

    //  emissive calibration: the emission -> lighting -> glow chain is
    // driven by the materials-LUT emissive_intensity column (row 2, v=0.625),
    // rescaled from the normalized LUT value. A material with intensity 0 emits
    // no glow; the glow scales LINEARLY and MONOTONICALLY with intensity so the
    // authored value maps predictably to on-screen luminance (calibration table
    // documents the transfer curve). The crystal's prismatic look is preserved
    // as the glow's color/shape; intensity only scales magnitude.
    // material LUT widened to 4 rows; row 2 = emissive (center via lutRowV).
    float emissiveIntensity = texture(u_materialLUT, vec2(float(MaterialID) / 255.0, lutRowV(2))).r * u_emissiveLutScale;
    if (emissiveIntensity > 0.0) {
        // Inner magical glow
        float glowPulse = sin(u_time * 2.0) * 0.3 + 0.7;
        vec3 magicColor = vec3(0.6, 0.8, 1.0) * glowPulse * 0.8;

        // Fresnel-based edge lighting
        float fresnel = pow(1.0 - max(0.0, dot(V, Normal)), 3.0);
        vec3 edgeGlow = vec3(0.4, 0.7, 1.0) * fresnel * 1.5;

        // Prismatic dispersion effect
        float dispersion = sin(u_time * 1.5 + FragPos.x * 0.1 + FragPos.z * 0.15);
        vec3 prismColors = vec3(
            0.8 + 0.4 * sin(dispersion),
            0.6 + 0.4 * sin(dispersion + 2.0),
            1.0 + 0.2 * sin(dispersion + 4.0)
        );

        // Energy field around crystals
        float energyField = abs(sin(u_time * 3.0 + length(FragPos) * 0.05)) * 0.3;

        crystalGlow = magicColor + edgeGlow + prismColors * energyField * 0.2;
        // Linear, monotonic scale by authored emissive_intensity (the old fixed
        // 1.5 HDR boost is now intensity 1.0 -> 1.5x; transfer curve documented).
        crystalGlow *= 1.5 * emissiveIntensity;
    }

    // Aetheric emissive tap. Sample the field at the fragment's world
    // XZ (FragPos is world-space here, as the crystal glow above uses it) and add
    // an additive glow. uv outside [0,1] (beyond the streamed grid) contributes
    // nothing. Fully gated by u_aetherActive so the default path is unchanged.
    vec3 aetherGlow = vec3(0.0);
    float aetherLocal = 0.0; // Shared with the material modulation below.
    if (u_aetherActive > 0.5) {
        vec2 auv = (FragPos.xz - u_aetherFieldWorldOrigin) * u_aetherFieldInvWorldSpan;
        if (auv.x >= 0.0 && auv.x <= 1.0 && auv.y >= 0.0 && auv.y <= 1.0) {
            float aether = max(0.0, texture(u_aetherField, auv).r);
            aetherLocal = aether;
            // polarity tint (dual RG32F tap only). Inactive -> the
            // glow color passes through untouched (pixel-identical).
            vec3 glowColor = u_aetherGlowColor;
            if (u_aetherPolarityActive > 0.5) {
                float pol = clamp(texture(u_aetherField, auv).g, -1.0, 1.0);
                vec3 pole = pol >= 0.0 ? u_aetherPolarityColorPos: u_aetherPolarityColorNeg;
                glowColor = mix(glowColor, pole, abs(pol));
            }
            aetherGlow = aether * glowColor * u_aetherGlowIntensity;
        }
    }

    //  ( -6): the local aether modulates emissive
    // MATERIALS (the crystal glow) multiplicatively. Modulation 0.0 (default)
    // multiplies by exactly 1.0 -> pixel-identical; increasing it can only
    // raise the emissive term (aetherLocal >= 0), so the probe luminance is
    // monotonic non-decreasing (the RenderSmokeTest contract).
    crystalGlow *= (1.0 + aetherLocal * u_aetherMaterialModulation);

    // --- Final Color Composition ---
    // terrain PBR: split the flat sky ambient into energy-conserving diffuse +
    // specular. u_skyAmbientColor is ALREADY an irradiance (kAmbientIrradianceScale
    // = PI matches SUN_IRRADIANCE_SCALE) — do NOT re-multiply by PI here or the
    // ambient double-counts and blows past the ACES knee. The specular lobe gives
    // metals/low-roughness surfaces a believable environment sheen at grazing
    // angles (previously flat). Non-metals at normal incidence keep ~96% of the
    // old diffuse (kD ~ 1 - 0.04) plus a faint rim, so the visual floor holds.
    // Far-water (MaterialID 200) has F0=0 + roughness 1.0 => F_amb=0 => specular 0
    // and diffuse unchanged, preserving the matte sky-tint sheet.
    float NdotV_amb = max(dot(Normal, V), 0.0);
    vec3 F_amb = fresnelSchlickRoughness(NdotV_amb, F0, Roughness);
    vec3 kD_amb = (vec3(1.0) - F_amb) * (1.0 - Metallic);
    //  realistic lighting: HEMISPHERIC ambient instead of a flat fill that
    // washed every face equally (the "flat / underwater" look). Up-facing surfaces
    // receive the full sky irradiance; down/side faces fade to a dimmer, warmer
    // ground-bounce term. This gives slopes, creases and undersides real form and
    // lets non-sun-facing geometry read darker, restoring directional contrast.
    float hemi = clamp(0.5 + 0.5 * Normal.y, 0.0, 1.0); // 1 = up (sky), 0 = down
    vec3 groundBounce = u_skyAmbientColor * vec3(0.32, 0.29, 0.25); // dim, warm
    vec3 hemiAmbient = mix(groundBounce, u_skyAmbientColor, hemi);
    vec3 ambientDiffuse  = kD_amb * Albedo * hemiAmbient;
    vec3 ambientSpecular = F_amb * hemiAmbient;
    // Long-range sky visibility: roofed-over (cave/overhang) fragments lose the
    // flat sky-ambient fill so deep interiors go near-black and any point light
    // placed there reads as a real pool of light. GATED (default OFF) -> the
    // skyVis multiplier is exactly 1.0 when u_caveAmbientOcclusion==0, so the
    // shipped path is pixel-identical. Applied to AMBIENT ONLY; Lo (sun/moon/
    // point) is untouched.
    float skyVis = 1.0;
    if (u_caveAmbientOcclusion > 0.5) {
        skyVis = computeSkyVisibility(viewPos, viewNormal);
    }
    vec3 ambient = (ambientDiffuse + ambientSpecular) * ao * skyVis;

    // EMISSIVE GAMEPLAY MARKERS: PlantProcgenPass beacons (creatures/foragers/food/
    // nest/crystals) pack a per-fragment emissive strength into gNormalMaterial.b
    // (0.0 for all normal geometry, so this term vanishes everywhere else). The glow
    // uses the marker's OWN albedo (already species-tinted) so each beacon glows its
    // own colour, and is faded with daylight (sunLum, computed above) so it BLAZES in
    // dark caves / at night but stays subtle in full sun.
    float markerEmissive = normalData.b;
    vec3 markerGlow = vec3(0.0);
    if (markerEmissive > 0.0) {
        float daylight = smoothstep(0.0, 0.30, sunLum);       // ~0 night -> 1 day
        float dayFade  = mix(1.0, 0.18, daylight);            // dim, never off, by day
        markerGlow = Albedo * (markerEmissive * 6.0 * dayFade);
    }

    vec3 color = ambient + Lo + caustics + crystalGlow + aetherGlow + markerGlow; // + emissive markers

    //  (seabed waterline terracing de-band): the far seabed
    // is a height-quantized heightfield (kFarLodHeightQuantScale = 1/16 m). Where
    // the gently-sloping seabed crosses the waterline the 1/16 m steps read as
    // horizontal terraces, and bare sun-bright sand shows above them. Tint any
    // below-sea-level upward-facing terrain toward the deep-water color with
    // depth: the terrace steps dissolve into a smooth depth gradient (the banding
    // is HIDDEN, per design  "or hide (depth-fade)") and the submerged seabed
    // reads as water rather than bright sand. Pure post-shade blend in linear
    // space; no tile-byte change (world_hash + far-tile bytes untouched). The
    // far-water sheet (id 200) and live water (id 7, discarded in g_buffer) are
    // excluded - they carry their own surface look.
    if (MaterialID != 200u && Normal.y > 0.3) {
        float depth = u_sea_level - FragPos.y; // >0 below the waterline
        if (depth > 0.0) {
            // Deep-water linear color matched to the far-water sheet albedo run
            // through the lit chain (g_buffer.frag case-200 ~vec3(0.018,0.065,
            // 0.11)). 0..1 fade reaches near-full tint by ~2.5 m of depth, so the
            // shoreline keeps a thin readable wet-sand lip and deeper seabed goes
            // fully water-toned (no terrace steps).
            vec3 deepWater = vec3(0.015, 0.05, 0.085);
            float t = clamp(depth / 2.5, 0.0, 0.92);
            color = mix(color, deepWater, t);
        }
    }

    // the lightning light-pulse + bolt are injected by a dedicated
    // FULL-SCREEN overlay (lightning_overlay.frag) drawn AFTER the skybox, so the
    // flash composites over BOTH the lit terrain and the sky. (The lighting pass
    // shades only G-buffer geometry; the skybox later overwrites sky pixels, so a
    // sky-spanning bolt injected here would be painted over.) See LightingPass.

    //  realistic-lighting grade: exposure lifts midtones BEFORE the filmic
    // curve so the noon scene is no longer dim/washed.
    color *= u_exposure;

    // Enhanced HDR tone mapping for magical effects
    // Use filmic tone mapping to preserve magical highlights
    color = color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14);

    //  grade: restore saturation + contrast the flat ambient/filmic wash out,
    // and apply a subtle sun warmth tint. Applied in tonemapped [0,1] space.
    float gradeLuma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(gradeLuma), color, u_saturation);
    color = clamp((color - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Cinematic split-tone: cool shadows -> warm highlights, blended by luma.
    float toneT = smoothstep(0.12, 0.88, gradeLuma);
    vec3 splitTint = mix(u_shadowTint, u_highlightTint, toneT);
    color = mix(color, color * splitTint, clamp(u_splitToneStrength, 0.0, 1.0));
    color *= u_lightWarmth;
    color = clamp(color, 0.0, 1.0);

    // Gamma correction
    color = pow(color, vec3(1.0/2.2));

    // tiny black floor on LIT GEOMETRY so a deep-shadow surface (e.g. dark rock
    // in an eroded crevice at the camera's feet, now darker with the real 1024 PBR
    // albedo) never renders a PURE-black (<=2/255) pixel. The PlayerView void-cluster
    // gate flags contiguous max(r,g,b)<=2 regions as geometry holes; legitimately-dark
    // lit terrain is not a hole. This pass shades only G-buffer geometry (the skybox
    // overwrites sky pixels afterward), and a TRUE hole shows the skybox dome (not <=2
    // black), so the floor cannot mask a real void. ~4/255 is imperceptible and keeps
    // pixels within the near-black (<=14) band the night-darkness gates measure.
    color = max(color, vec3(4.0/255.0));

    FragColor = vec4(color, 1.0);
}
