#version 450 core
layout (location = 0) out vec3 gPosition;          // RGB16F: full view-space position
layout (location = 1) out vec4 gNormalMaterial;    // RGB10A2: Octahedral normal + material ID
layout (location = 2) out vec4 gAlbedoRoughness;   // RGBA8: RGB albedo + roughness
layout (location = 3) out vec2 gMetallicAO;        // RG16F: Metallic + AO
layout (location = 4) out vec2 gMotionVector;      // RG16F: screen motion (NDC delta); /TAAU

// Octahedral normal encoding functions
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}

vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy: octWrap(n.xy);
}

// Material lookup texture (256 x 4 rows, //). Row centers for a
// 4-tall NEAREST texture are v = (row+0.5)/4 = 0.125, 0.375, 0.625, 0.875:
//   row 0 (v=0.125): [metallic, roughness, ao, magical]
//   row 1 (v=0.375): [texture_layer/255, normal_layer/255, tiling/64, has_texture]
//   row 2 (v=0.625): [emissive_intensity/scale, albedo_scale, reserved] (emissive read by lighting)
//   row 3 (v=0.875): [albedo_tint.rgb, reserved]  ( dusty-warm palette palette)
uniform sampler2D u_materialLUT;
// single source of truth for the LUT row count. Row-center v-coords are
// derived from it so a future row-count bump can't leave stale literals that
// silently sample the wrong row under NEAREST filtering. MUST match
// RenderPipeline::init_material_lut ROWS.
const int LUT_ROWS = 4;
float lutRowV(int row) { return (float(row) + 0.5) / float(LUT_ROWS); }

//  triplanar terrain arrays (texture arrays, not bindless — design ).
// Layer indices come from the material LUT texture_layer / normal_layer columns.
uniform sampler2DArray u_terrainTextures;   // sRGB albedo
uniform sampler2DArray u_terrainNormals;    // tangent-space (OpenGL) normal maps
// terrain PBR roughness-map: per-material roughness-map array (linear; roughness in.r). When
// u_terrainRoughnessValid == 0 (a layer failed to load) the shader keeps the flat
// per-material scalar instead of a wrong constant.
uniform sampler2DArray u_terrainRoughness;
uniform int u_terrainRoughnessValid;

//  render-scale mip bias. When the scene renders at a reduced
// internal resolution (render_scale < 1.0) the implicit-derivative LOD picks a
// coarser mip, so material textures read blurry after upscale. A negative bias of
// log2(render_scale) restores near-native sharpness. The uniform defaults to 0.0
// and the client sets exactly log2(1.0)==0.0 at render_scale 1.0, so the
// `!= 0.0` guard below takes the VERBATIM original texture path there — the
// scale-1.0 output is byte-identical to the legacy build by construction (the
// whole-frame A/B gate proves dispatch determinism, not equivalence, so the no-op
// must hold structurally). Only material arrays (mipmapped) are biased; the NEAREST
// material LUT is not.
uniform float u_lodBias = 0.0;
vec4 sampleBias(sampler2DArray s, vec3 uvw) {
    return u_lodBias != 0.0 ? texture(s, uvw, u_lodBias): texture(s, uvw);
}

// View rotation (mat3 of the camera view matrix). The normal-mapped normal is
// perturbed in world space then rotated into view space here, so the G-buffer
// keeps storing a VIEW-SPACE octahedral normal (lighting pass unchanged).
uniform mat3 u_normalViewMatrix;

//  skinned (UV-mapped) texturing. Skinned creatures sample an albedo (and
// optional normal) layer by their mesh UVs instead of the terrain triplanar
// path. Layers < 0 disable it (terrain/static draws set these to -1).
uniform sampler2DArray u_skinnedTextures;
uniform int u_skinnedAlbedoLayer = -1;
uniform int u_skinnedNormalLayer = -1;
//  procedural creatures: per-creature albedo tint (linear RGB), set per skinned
// draw from the species base_color. Defaults to white so an unset/uninitialised creature
// renders exactly as authored; only multiplies the skinned albedo sample below, so no
// other (terrain/static) path is affected.
uniform vec3 u_albedo_tint = vec3(1.0);
uniform int u_alphaTest = 0; // 0 opaque, 1 texture-luma cutout, 2 procedural leaf silhouette.

// macro ROCK-on-steep-faces overlay is a TERRAIN-only macro-variation
// (natural cliffs read as scree). It costs vnoise + up to 3 extra triplanar samples per
// fragment. Instanced foliage/props (procgen trees/bushes/rocks) used materials 1-3 and so
// paid this every leaf/bark fragment — heavily overdrawn — for no visual benefit (a leaf
// card should never sample rock). Terrain sets this to 1 (byte-identical to before); the
// instanced static-mesh path sets it to 0, which skips the branch entirely..
uniform int u_macroRockOverlay = 1;

// force the cheap FLAT-material path (skip ALL triplanar sampling) for the
// procedural foliage cards (leaves + bushes). These are alpha-keyed, heavily overdrawn, and
// get their colour almost entirely from the per-instance green tint — the world-projected
// grass triplanar (6-9 texture-array samples/fragment) is invisible on a fluttering leaf card
// but was the single largest G-buffer cost in the forest. The flat grass base colour × Tint
// reads the same at the densities this draws. Terrain/bark/rock keep triplanar (set to 0).
uniform int u_forceFlat = 0;

//  far-LOD: view-space radius (meters) inside which far-region mesh
// fragments are discarded - the live chunk ring owns that space (live wins;
// the under-terrain far fill must not show through live LOD seam cracks at
// close range, where shadow/SSAO render it near-black). Default 0.0 disables
// the clip; live chunk and static mesh draws never set it.
uniform float u_farClipInnerRadius;

// Worldgen-preview far-field: a CENTER-relative (world-space XZ) variant of the
// inner clip above. The preview is an external orbit camera looking at a fixed
// diorama centre, so the camera-relative u_farClipInnerRadius would discard a
// moving 176 m disc around the orbiting camera (wrong). Instead, far-region
// fragments whose world XZ lies within u_farPreviewInnerRadius of
// u_farPreviewCenterXZ are discarded — the small bounded live slice owns that
// space, so the coarse far mesh cannot poke through it. Default radius 0.0
// disables it (every game draw + live/static mesh draw); only the preview far
// draws set it > 0.
uniform vec2 u_farPreviewCenterXZ;
uniform float u_farPreviewInnerRadius;

//  (TAAU): previous-frame view-projection + inverse screen size for motion vectors, plus this
// frame's sub-pixel projection jitter (removed from the current position so motion stays jitter-free).
uniform mat4 u_prev_view_proj;
uniform vec2 u_inv_screen_size;
uniform vec2 u_jitter_ndc;

// Input from the vertex shader, with "flat" interpolation for the integer ID
in VS_OUT {
    vec3 FragPos;      // VIEW SPACE
    vec3 Normal;       // VIEW SPACE
    vec3 WorldPos;     // WORLD SPACE (triplanar projection)
    vec3 WorldNormal;  // WORLD SPACE
    vec2 UV;           // mesh UV (skinned/static texturing, )
    flat uint MaterialID;
    vec3 Tint;         // per-instance albedo tint (1,1,1 = no-op)
    vec3 PrevWorldPos; //  TAAU: world pos with PREVIOUS-frame wind sway (== WorldPos for static)
} fs_in;

//  terrain visual-fidelity (visual-fidelity warm palette floor),: the 256px terrain
// textures read muddy/low-contrast. Amplify the EXISTING high-frequency detail
// (unsharp mask vs a mip-blurred base) so the surface de-muds + carries visible
// detail, and boost normal-map strength so relief catches light. Mean-preserving
// (no exposure shift); auto-fades at distance where the texture is minified
// (sharp ~= blur), so it lifts the visible near/mid terrain. Cheap (3 extra
// textureLod + a few mults).
const float kDetailBlurLod = 3.0;   // mip level used as the unsharp low-freq base
const float kDetailGain    = 1.4;   // high-freq amplification (>1 sharpens). Grass overhaul: lowered
                                    // 2.2->1.4 so the terrain (esp. grass) stops reading as a wavy/liquid
                                    // "algae" surface — softer relief, less amplified ripple..
// NOTE: a normal-map strength boost was tried here and dropped — it amplifies a
// pre-existing sky-ambient blue-speckle LIGHTING artifact on terrain facets without
// adding meaningful detail (the albedo unsharp carries the gain). The speckle is a
// separate lighting implementation note (terrain lighting contract).

// Triplanar blend weights from a world-space normal (sharpened, normalized).
vec3 triplanar_weights(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));
    return w / max(w.x + w.y + w.z, 1e-4);
}

//  macro material variation: cheap spatially-coherent value noise to jitter the
// slope/height material boundaries so they read as natural transitions, not clean
// contour lines. Deterministic in WORLD space (no temporal shimmer under motion).
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0,0,0)), n100 = hash13(i + vec3(1,0,0));
    float n010 = hash13(i + vec3(0,1,0)), n110 = hash13(i + vec3(1,1,0));
    float n001 = hash13(i + vec3(0,0,1)), n101 = hash13(i + vec3(1,0,1));
    float n011 = hash13(i + vec3(0,1,1)), n111 = hash13(i + vec3(1,1,1));
    float nx00 = mix(n000, n100, f.x), nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x), nx11 = mix(n011, n111, f.x);
    return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}

// Triplanar albedo sample from the terrain array, with unsharp detail amplification.
vec3 triplanar_albedo(vec3 worldPos, vec3 weights, float layer, float scale) {
    vec2 uv_x = worldPos.zy * scale;
    vec2 uv_y = worldPos.xz * scale;
    vec2 uv_z = worldPos.xy * scale;
    vec3 cx = sampleBias(u_terrainTextures, vec3(uv_x, layer)).rgb;
    vec3 cy = sampleBias(u_terrainTextures, vec3(uv_y, layer)).rgb;
    vec3 cz = sampleBias(u_terrainTextures, vec3(uv_z, layer)).rgb;
    vec3 sharp = cx * weights.x + cy * weights.y + cz * weights.z;
    // Mip-blurred base for the unsharp mask (mean-preserving high-freq boost).
    // Track the fragment's ACTUAL mip and keep the blur a fixed number of mips
    // ABOVE it, instead of pinning the blur at kDetailBlurLod. Pinned, the sharp
    // sample minified at distance until sharp~=blur and the detail gain collapsed
    // -> flat plastic terrain at mid/far range. Tracking the mip preserves a
    // constant sharp-vs-blur separation across the whole view distance, so surface
    // detail survives to the horizon. Mean-preserving (unsharp), so the calibration
    // luminance is unchanged near camera.
    float baseLod = max(textureQueryLod(u_terrainTextures, uv_y).y, 0.0);
    // Keep the unsharp separation constant relative to the (possibly biased) sharp
    // sample: +u_lodBias is added last so the original subexpression is preserved
    // verbatim (x + 0.0 == x at render_scale 1.0).
    float blurLod = baseLod + kDetailBlurLod + u_lodBias;
    vec3 bx = textureLod(u_terrainTextures, vec3(uv_x, layer), blurLod).rgb;
    vec3 by = textureLod(u_terrainTextures, vec3(uv_y, layer), blurLod).rgb;
    vec3 bz = textureLod(u_terrainTextures, vec3(uv_z, layer), blurLod).rgb;
    vec3 blur = bx * weights.x + by * weights.y + bz * weights.z;
    return clamp(blur + (sharp - blur) * kDetailGain, 0.0, 1.0);
}

// Triplanar tangent-space normal sample, reoriented to world space via the
// whiteout blend (Ben Golus): perturb each axis projection's geometric normal
// by its sampled tangent-space normal, then blend by the triplanar weights.
vec3 triplanar_normal(vec3 worldPos, vec3 geomN, vec3 weights, float layer, float scale) {
    vec2 uv_x = worldPos.zy * scale;
    vec2 uv_y = worldPos.xz * scale;
    vec2 uv_z = worldPos.xy * scale;
    vec3 nx = sampleBias(u_terrainNormals, vec3(uv_x, layer)).xyz * 2.0 - 1.0;
    vec3 ny = sampleBias(u_terrainNormals, vec3(uv_y, layer)).xyz * 2.0 - 1.0;
    vec3 nz = sampleBias(u_terrainNormals, vec3(uv_z, layer)).xyz * 2.0 - 1.0;
    // Whiteout blend: add the geometric normal into the z of each tangent-space
    // sample, swizzle into world axes, then weight-blend.
    vec3 wx = vec3(nx.xy + geomN.zy, abs(nx.z) * geomN.x);
    vec3 wy = vec3(ny.xy + geomN.xz, abs(ny.z) * geomN.y);
    vec3 wz = vec3(nz.xy + geomN.xy, abs(nz.z) * geomN.z);
    vec3 worldN =
        wx.zyx * weights.x +
        wy.xzy * weights.y +
        wz.xyz * weights.z;
    return normalize(worldN);
}

// terrain PBR roughness-map: triplanar per-texel roughness (.r), same projection as albedo.
float triplanar_roughness(vec3 worldPos, vec3 weights, float layer, float scale) {
    vec2 uv_x = worldPos.zy * scale;
    vec2 uv_y = worldPos.xz * scale;
    vec2 uv_z = worldPos.xy * scale;
    float rx = sampleBias(u_terrainRoughness, vec3(uv_x, layer)).r;
    float ry = sampleBias(u_terrainRoughness, vec3(uv_y, layer)).r;
    float rz = sampleBias(u_terrainRoughness, vec3(uv_z, layer)).r;
    // Rock-speckle fix (render-only): the AmbientCG roughness plates carry a sparse
    // scatter of near-zero (near-mirror) texels. At the noon sun those collapse GGX
    // a2 -> ~0 and spike a pinpoint white specular highlight = the BRIGHT SPECKS on
    // rock/terrain. Raise the floor to 0.35 (matte stone) so micro-variation survives
    // (0.35..1.0) without the mirror spikes. Shared by all terrain mats (all matte). No world_hash.
    return clamp(rx * weights.x + ry * weights.y + rz * weights.z, 0.35, 1.0);
}

void main()
{
    // Public procedural trees have no leaf-card texture. Give their cards an
    // explicit silhouette instead of drawing opaque rectangular terrain patches.
    bool proceduralLeaf = u_alphaTest == 2;
    if (proceduralLeaf) {
        vec2 leaf = fs_in.UV * 2.0 - 1.0;
        leaf.x *= 1.0 + 0.10 * sin(fs_in.UV.y * 22.0);
        if (dot(leaf, leaf) > 1.0) discard;
    }
    if (fs_in.MaterialID == 7u) { // Water
        discard;
    }
    if (u_farClipInnerRadius > 0.0 &&
        dot(fs_in.FragPos, fs_in.FragPos) < u_farClipInnerRadius * u_farClipInnerRadius) {
        discard; // far-region fragment inside the live ring: live wins
    }
    if (u_farPreviewInnerRadius > 0.0) {
        // Worldgen-preview far-field: discard far fragments inside the fixed
        // diorama slice (center-relative, world XZ). Keeps the coarse far mesh
        // from poking through the fine live slice for the external orbit camera.
        vec2 dxz = fs_in.WorldPos.xz - u_farPreviewCenterXZ;
        if (dot(dxz, dxz) < u_farPreviewInnerRadius * u_farPreviewInnerRadius) {
            discard;
        }
    }

    // --- Material properties from lookup texture ---
    float matIndex = float(fs_in.MaterialID) / 255.0;
    // sample at the row centers derived from LUT_ROWS (no magic literals).
    vec4 matProps = texture(u_materialLUT, vec2(matIndex, lutRowV(0))); // row 0
    vec4 texInfo  = texture(u_materialLUT, vec2(matIndex, lutRowV(1))); // row 1
    // row 2 G channel is the per-material albedo multiplier
    // (default 1.0). Applied to the baked textured albedo below so a physically-
    // bright photographic texture (the noon sun-bright sand flat) calibrates to a
    // natural lit tone that survives tonemapping below the ACES clip. Render-only.
    float albedoScale = texture(u_materialLUT, vec2(matIndex, lutRowV(2))).g; // row 2
    //  dusty-warm palette palette: row 3 RGB is the per-material warm albedo tint
    // (default 1,1,1 = no-op). Applied alongside albedoScale in the triplanar
    // branch only, so flat/UV(model) paths are untouched. Render-only content
    // base-color nudge; distinct from the post LUMIN_GRADE / LUMIN_ATMOS stages.
    vec3 albedoTint = texture(u_materialLUT, vec2(matIndex, lutRowV(3))).rgb; // row 3

    float metallic = matProps.r;
    float roughness = matProps.g;
    float ao = matProps.b;

    // --- Base (flat) albedo per material id ---
    vec3 albedo = vec3(0.7);
    switch (fs_in.MaterialID) {
        case 1u: albedo = vec3(0.5); break;                    // Stone
        case 2u: albedo = vec3(0.3, 0.15, 0.05); break;        // Soil
        case 3u: albedo = vec3(0.2, 0.6, 0.15); break;         // Grass
        case 4u: albedo = vec3(0.9, 0.8, 0.5); break;          // Sand
        case 6u: albedo = vec3(0.85, 0.95, 1.0); break;        // Luminous Crystal
        // flat far-water sheet. Deep-water albedo so the
        // far field reads as water past the live water ring, WITHOUT the live
        // water.frag reflection/caustic pipeline (too costly and unnecessary at
        // kilometer range). Material id 200 (FarLodSystem::kFarWaterMaterialId).
        //
        // the old albedo (~0.17,0.26,0.36 linear) was
        // a mid-bright sky-tinted blue. The sheet faces straight up, so at the
        // pinned noon sun it takes near-maximum sun irradiance; run through the
        // calibrated exposure chain (SUN_IRRADIANCE_SCALE = PI in lighting_pass)
        // every channel clipped past the ACES knee and the surface rendered flat
        // near-white (measured on-screen sRGB ~233,231,226 - warm-white, B BELOW
        // R) instead of blue. That produced a hard white/cyan seam against the
        // correctly-cyan live water.frag forward pass. The deep-water reflectance
        // of real open ocean is very low (broadband linear albedo well under
        // 0.05, blue-weighted); authoring the sheet at a genuinely deep-water
        // linear albedo keeps the lit surface below the ACES clip so it survives
        // tonemapping as deep blue (B markedly above R) and the live/far seam
        // becomes a soft tint step. Paired with the explicit matte water row in
        // RenderPipeline::init_material_lut (metallic 0, low roughness) so no
        // broad white specular lobe washes the channels back toward white.
        case 200u: albedo = vec3(0.018, 0.065, 0.11); break;
    }

    // --- Triplanar terrain texturing ---
    // Albedo and normal are baked into the G-buffer here so the lighting pass
    // sees the fully textured surface (and the normal-map perturbation feeds
    // shadows/specular). Gated by the LUT has_texture flag so crystal/water and
    // untextured ids keep their flat base color and geometric normal.
    vec3 worldN = normalize(fs_in.WorldNormal);
    bool textured = false;
    if (proceduralLeaf) {
        // The same leaf reflectance at every mesh LOD avoids a bright green
        // distance band. Keep ground textures and their normal maps off leaves.
        albedo = vec3(0.10, 0.24, 0.045);
        roughness = 0.85;
        metallic = 0.0;
        if (!gl_FrontFacing) worldN = -worldN;
        textured = true; // rotate the two-sided world normal into view space below
    } else if (u_skinnedAlbedoLayer >= 0) {
        // UV-mapped skinned/creature texturing. Samples the skinned
        // texture array by the mesh UVs; optionally perturbs the normal by a
        // tangent-derivative-free approximation (UV-space normal map, applied in
        // world space via the geometric normal as the z axis).
        // also the static-model lane (tree bark/leaf) — same UV sampling.
        albedo = sampleBias(u_skinnedTextures, vec3(fs_in.UV, float(u_skinnedAlbedoLayer))).rgb * u_albedo_tint;
        //  leaf cutout: the source leaf textures are RGB leaf-cards on a BLACK
        // background (no alpha), so key the cutout off luminance — the black inter-
        // leaf gaps are discarded, leaving the lit leaf shapes. NOTE: the array is
        // SRGB8, so `albedo` here is LINEAR; medium-green leaves are only ~0.06-0.1
        // linear luma, so the threshold must be low (black bg is ~0). Only active
        // for alpha-test parts (leaves); opaque otherwise.
        if (u_alphaTest == 1) {
            float leafLuma = dot(albedo, vec3(0.299, 0.587, 0.114));
            if (leafLuma < 0.025) discard;
        }
        if (u_skinnedNormalLayer >= 0) {
            vec3 tn = sampleBias(u_skinnedTextures, vec3(fs_in.UV, float(u_skinnedNormalLayer))).xyz * 2.0 - 1.0;
            // Build an ad-hoc tangent basis from the geometric world normal so
            // the tangent-space perturbation maps into world space.
            vec3 up = abs(worldN.y) < 0.99 ? vec3(0.0, 1.0, 0.0): vec3(1.0, 0.0, 0.0);
            vec3 t = normalize(cross(up, worldN));
            vec3 b = cross(worldN, t);
            worldN = normalize(t * tn.x + b * tn.y + worldN * max(tn.z, 0.1));
        }
        textured = true;
    } else if (texInfo.a > 0.5 && u_forceFlat == 0) {
        float texLayer = floor(texInfo.r * 255.0 + 0.5);
        float normLayer = floor(texInfo.g * 255.0 + 0.5);
        float tiling = max(texInfo.b * 64.0, 0.0625);
        float scale = 1.0 / tiling; // repeats per world unit
        vec3 weights = triplanar_weights(worldN);
        float geomSlope = worldN.y; // up-facing-ness (1 flat, 0 vertical), pre normal-map
        vec3 baseAlbedo = triplanar_albedo(fs_in.WorldPos, weights, texLayer, scale);
        vec3 baseN      = triplanar_normal(fs_in.WorldPos, worldN, weights, normLayer, scale);
        // terrain PBR roughness-map: per-texel roughness from the AmbientCG roughness map
        // (replaces the flat per-material scalar). Falls back to the scalar when
        // the map array is unavailable.
        float baseRoughness = (u_terrainRoughnessValid == 1)
            ? triplanar_roughness(fs_in.WorldPos, weights, texLayer, scale): roughness;

        //  macro material variation (, no world_hash): overlay ROCK on
        // steep faces so natural terrain stops reading as one uniform olive material
        // (the visual-fidelity warm palette macro-variation lift). Only natural ground ids (Stone/Soil/Grass
        // = 1..3); calibrated Sand (4) and the flat far-water sheet (200) are untouched,
        // so the sand exposure calibration + the live/far water seam are unchanged. The
        // rock layers are read from the Stone (id 1) material row so this tracks
        // materials.json (no hardcoded layer index). Steep faces blend toward rock with
        // a world-space-noise-jittered boundary so cliffs read as natural scree, not a
        // clean contour line.
        // Perf (gbuffer §GPU): rockW = smoothstep(0.80+jitter, 0.50+jitter, geomSlope) is
        // exactly 0 for any geomSlope >= 0.89 (max jitter = (1-0.5)*0.18 = 0.09, so the upper
        // edge never exceeds 0.89). Flat ground (the majority of terrain pixels) therefore
        // contributed NOTHING but still paid for the vnoise + smoothstep every fragment. Gating
        // on geomSlope < 0.89 skips that work on flat terrain and is BYTE-IDENTICAL (the skipped
        // fragments had rockW==0 -> base material unchanged) -> no visual change, no gate re-bless.
        if (u_macroRockOverlay == 1 && geomSlope < 0.89 && fs_in.MaterialID >= 1u && fs_in.MaterialID <= 3u) {
            float jitter = (vnoise(fs_in.WorldPos * 0.05) - 0.5) * 0.18; // ~20 m break-up
            float rockW = smoothstep(0.80 + jitter, 0.50 + jitter, geomSlope); // steep -> rock
            if (rockW > 0.002) {
                vec4 rockInfo = texture(u_materialLUT, vec2(1.0/255.0, lutRowV(1))); // Stone row 1
                float rockTex   = floor(rockInfo.r * 255.0 + 0.5);
                float rockNrm   = floor(rockInfo.g * 255.0 + 0.5);
                float rockScale = 1.0 / max(rockInfo.b * 64.0, 0.0625);
                vec3 rockAlbedo = triplanar_albedo(fs_in.WorldPos, weights, rockTex, rockScale);
                vec3 rockN      = triplanar_normal(fs_in.WorldPos, worldN, weights, rockNrm, rockScale);
                baseAlbedo = mix(baseAlbedo, rockAlbedo, rockW);
                baseN      = normalize(mix(baseN, rockN, rockW));
                if (u_terrainRoughnessValid == 1) {
                    float rockRough = triplanar_roughness(fs_in.WorldPos, weights, rockTex, rockScale);
                    baseRoughness = mix(baseRoughness, rockRough, rockW);
                }
            }
        }
        worldN = baseN;
        // per-material albedo calibration on the textured
        // terrain path (live AND far-LOD sand both sample this triplanar branch -
        // sand carries has_texture). Default scale 1.0 is a no-op (byte-identical)
        // for every unscaled id. Confined to the triplanar branch so flat-material
        // and skinned paths are untouched (no spurious scaling of the case-switch
        // base colors). Brings the noon sun-bright sand flat down to a natural lit
        // tone below the ACES clip.
        //  dusty-warm palette palette: warm albedo tint (default 1,1,1 -> byte-identical).
        albedo = baseAlbedo * albedoScale * albedoTint;
        roughness = baseRoughness; // terrain PBR roughness-map: per-texel terrain roughness
        textured = true;
    }

    // MACRO ALBEDO VARIATION (rendering contract, all-distance terrain detail): the
    // rock/soil triplanar textures mip to a flat uniform tone at vista distance, so
    // terrain — and cave walls especially — read as a single flat brown. Modulate
    // albedo by a low-frequency WORLD-SPACE value noise: computed analytically per
    // fragment, it does NOT mip away, so terrain keeps organic large-scale tonal
    // variation at ANY distance (the visual-fidelity macro-variation lift). Two octaves,
    // natural-ground ids only (1..5; crystal/water/far-water untouched), subtle
    // +-12% so it reads as natural mottling, not blotches. Render-only; the mesh and
    // world_hash are untouched.
    if (fs_in.MaterialID >= 1u && fs_in.MaterialID <= 5u) {
        float macro  = vnoise(fs_in.WorldPos * 0.012) * 0.6 + vnoise(fs_in.WorldPos * 0.045) * 0.4;
        float macroH = vnoise(fs_in.WorldPos * 0.020 + vec3(31.7));
        albedo *= (1.0 + (macro - 0.5) * 0.45); // +-22% brightness mottle
        // Warm<->cool hue mottle so rock/soil reads as VARIED stone (iron/ochre vs
        // grey/slate) instead of one flat mud tone — the part that actually breaks
        // the uniform-brown look at distance.
        albedo *= mix(vec3(1.10, 1.02, 0.90), vec3(0.90, 0.96, 1.08), macroH);
    }

    // --- G-Buffer output ---
    gPosition = fs_in.FragPos;

    // The flat-shaded path keeps the interpolated view-space normal exactly as
    // before (byte-identical for untextured ids); the textured path rotates the
    // normal-mapped world normal into view space so the encoding stays uniform.
    vec3 viewN = textured ? normalize(u_normalViewMatrix * worldN)
: normalize(fs_in.Normal);
    vec2 encoded_normal = encode_octahedral(viewN);
    float material_id_normalized = float(fs_in.MaterialID) / 255.0;
    gNormalMaterial = vec4(encoded_normal * 0.5 + 0.5, 0.0, material_id_normalized);

    gAlbedoRoughness = vec4(albedo * fs_in.Tint, roughness);
    gMetallicAO = vec2(metallic, ao);
    //  (TAAU): screen-space motion vector = current screen pos - reprojected previous pos.
    // Current pos is exact from gl_FragCoord (NDC); previous pos reprojects this surface point's
    // ABSOLUTE world position through last frame's view-proj. Captures camera/rigid motion
    // (wind/skinned animation reproject as static -> deferred follow-on). The w<=0 guard (point
    // behind the previous camera, or an unset identity prev-VP on frame 0) yields zero motion.
    vec2 currNdc = gl_FragCoord.xy * u_inv_screen_size * 2.0 - 1.0 - u_jitter_ndc; // remove jitter
    // reproject the PREVIOUS-frame world position (wind sway included) through the previous
    // view-proj. For static geometry PrevWorldPos == WorldPos, so this is identical to camera-only
    // reprojection; for wind-swayed foliage it cancels the per-frame sway delta (no tree-top ghosting).
    vec4 prevClip = u_prev_view_proj * vec4(fs_in.PrevWorldPos, 1.0);
    gMotionVector = (prevClip.w > 1e-5) ? (currNdc - prevClip.xy / prevClip.w): vec2(0.0);
}
