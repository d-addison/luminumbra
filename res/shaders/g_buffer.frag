#version 450 core
layout (location = 0) out vec3 gPosition;          // RGB16F: full view-space position
layout (location = 1) out vec4 gNormalMaterial;    // RGB10A2: Octahedral normal + material ID
layout (location = 2) out vec4 gAlbedoRoughness;   // RGBA8: RGB albedo + roughness
layout (location = 3) out vec2 gMetallicAO;        // RG16F: Metallic + AO

// Octahedral normal encoding functions
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}

vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

// Material lookup texture (256 x 3 rows, T-I4-7/T-I4-9). Row centers for a
// 3-tall NEAREST texture are v = 1/6, 1/2, 5/6:
//   row 0 (v=0.1667): [metallic, roughness, ao, magical]
//   row 1 (v=0.5):    [texture_layer/255, normal_layer/255, tiling/64, has_texture]
//   row 2 (v=0.8333): [emissive_intensity/scale, reserved...]  (read by lighting)
uniform sampler2D u_materialLUT;

// T-I4-7 triplanar terrain arrays (texture arrays, not bindless — design §10).
// Layer indices come from the material LUT texture_layer / normal_layer columns.
uniform sampler2DArray u_terrainTextures;   // sRGB albedo
uniform sampler2DArray u_terrainNormals;    // tangent-space (OpenGL) normal maps

// View rotation (mat3 of the camera view matrix). The normal-mapped normal is
// perturbed in world space then rotated into view space here, so the G-buffer
// keeps storing a VIEW-SPACE octahedral normal (lighting pass unchanged).
uniform mat3 u_normalViewMatrix;

// T-I4-8 skinned (UV-mapped) texturing. Skinned creatures sample an albedo (and
// optional normal) layer by their mesh UVs instead of the terrain triplanar
// path. Layers < 0 disable it (terrain/static draws set these to -1).
uniform sampler2DArray u_skinnedTextures;
uniform int u_skinnedAlbedoLayer = -1;
uniform int u_skinnedNormalLayer = -1;

// T-I3-9 far-LOD: view-space radius (meters) inside which far-region mesh
// fragments are discarded - the live chunk ring owns that space (live wins;
// the under-terrain far fill must not show through live LOD seam cracks at
// close range, where shadow/SSAO render it near-black). Default 0.0 disables
// the clip; live chunk and static mesh draws never set it.
uniform float u_farClipInnerRadius;

// Input from the vertex shader, with "flat" interpolation for the integer ID
in VS_OUT {
    vec3 FragPos;      // VIEW SPACE
    vec3 Normal;       // VIEW SPACE
    vec3 WorldPos;     // WORLD SPACE (triplanar projection)
    vec3 WorldNormal;  // WORLD SPACE
    vec2 UV;           // mesh UV (skinned/static texturing, T-I4-8)
    flat uint MaterialID;
} fs_in;

// T-I6 terrain visual-fidelity (BF4/BF1 floor), RENDER-ONLY: the 256px terrain
// textures read muddy/low-contrast. Amplify the EXISTING high-frequency detail
// (unsharp mask vs a mip-blurred base) so the surface de-muds + carries visible
// detail, and boost normal-map strength so relief catches light. Mean-preserving
// (no exposure shift); auto-fades at distance where the texture is minified
// (sharp ~= blur), so it lifts the visible near/mid terrain. Cheap (3 extra
// textureLod + a few mults).
const float kDetailBlurLod = 3.0;   // mip level used as the unsharp low-freq base
const float kDetailGain    = 2.2;   // high-freq amplification (>1 sharpens)
// NOTE: a normal-map strength boost was tried here and dropped — it amplifies a
// pre-existing sky-ambient blue-speckle LIGHTING artifact on terrain facets without
// adding meaningful detail (the albedo unsharp carries the gain). The speckle is a
// separate lighting follow-up (terrain-fidelity-plan.md).

// Triplanar blend weights from a world-space normal (sharpened, normalized).
vec3 triplanar_weights(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));
    return w / max(w.x + w.y + w.z, 1e-4);
}

// T-I6 macro material variation: cheap spatially-coherent value noise to jitter the
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
    vec3 cx = texture(u_terrainTextures, vec3(uv_x, layer)).rgb;
    vec3 cy = texture(u_terrainTextures, vec3(uv_y, layer)).rgb;
    vec3 cz = texture(u_terrainTextures, vec3(uv_z, layer)).rgb;
    vec3 sharp = cx * weights.x + cy * weights.y + cz * weights.z;
    // Mip-blurred base for the unsharp mask (mean-preserving high-freq boost).
    vec3 bx = textureLod(u_terrainTextures, vec3(uv_x, layer), kDetailBlurLod).rgb;
    vec3 by = textureLod(u_terrainTextures, vec3(uv_y, layer), kDetailBlurLod).rgb;
    vec3 bz = textureLod(u_terrainTextures, vec3(uv_z, layer), kDetailBlurLod).rgb;
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
    vec3 nx = texture(u_terrainNormals, vec3(uv_x, layer)).xyz * 2.0 - 1.0;
    vec3 ny = texture(u_terrainNormals, vec3(uv_y, layer)).xyz * 2.0 - 1.0;
    vec3 nz = texture(u_terrainNormals, vec3(uv_z, layer)).xyz * 2.0 - 1.0;
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

void main()
{
    if (fs_in.MaterialID == 7u) { // Water
        discard;
    }
    if (u_farClipInnerRadius > 0.0 &&
        dot(fs_in.FragPos, fs_in.FragPos) < u_farClipInnerRadius * u_farClipInnerRadius) {
        discard; // far-region fragment inside the live ring: live wins
    }

    // --- Material properties from lookup texture ---
    float matIndex = float(fs_in.MaterialID) / 255.0;
    vec4 matProps = texture(u_materialLUT, vec2(matIndex, 0.16667)); // row 0
    vec4 texInfo  = texture(u_materialLUT, vec2(matIndex, 0.5));     // row 1
    // T-I5b-5-water-backlog: row 2 G channel is the per-material albedo multiplier
    // (default 1.0). Applied to the baked textured albedo below so a physically-
    // bright photographic texture (the noon sun-bright sand flat) calibrates to a
    // natural lit tone that survives tonemapping below the ACES clip. Render-only.
    float albedoScale = texture(u_materialLUT, vec2(matIndex, 0.83333)).g; // row 2

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
        // T-I4-DR-far-water-sheet: flat far-water sheet. Deep-water albedo so the
        // far field reads as water past the live water ring, WITHOUT the live
        // water.frag reflection/caustic pipeline (too costly and unnecessary at
        // kilometer range). Material id 200 (FarLodSystem::kFarWaterMaterialId).
        //
        // T-I4-DR-far-water-exposure: the old albedo (~0.17,0.26,0.36 linear) was
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

    // --- Triplanar terrain texturing (T-I4-7) ---
    // Albedo and normal are baked into the G-buffer here so the lighting pass
    // sees the fully textured surface (and the normal-map perturbation feeds
    // shadows/specular). Gated by the LUT has_texture flag so crystal/water and
    // untextured ids keep their flat base color and geometric normal.
    vec3 worldN = normalize(fs_in.WorldNormal);
    bool textured = false;
    if (u_skinnedAlbedoLayer >= 0) {
        // T-I4-8: UV-mapped skinned/creature texturing. Samples the skinned
        // texture array by the mesh UVs; optionally perturbs the normal by a
        // tangent-derivative-free approximation (UV-space normal map, applied in
        // world space via the geometric normal as the z axis).
        albedo = texture(u_skinnedTextures, vec3(fs_in.UV, float(u_skinnedAlbedoLayer))).rgb;
        if (u_skinnedNormalLayer >= 0) {
            vec3 tn = texture(u_skinnedTextures, vec3(fs_in.UV, float(u_skinnedNormalLayer))).xyz * 2.0 - 1.0;
            // Build an ad-hoc tangent basis from the geometric world normal so
            // the tangent-space perturbation maps into world space.
            vec3 up = abs(worldN.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 t = normalize(cross(up, worldN));
            vec3 b = cross(worldN, t);
            worldN = normalize(t * tn.x + b * tn.y + worldN * max(tn.z, 0.1));
        }
        textured = true;
    } else if (texInfo.a > 0.5) {
        float texLayer = floor(texInfo.r * 255.0 + 0.5);
        float normLayer = floor(texInfo.g * 255.0 + 0.5);
        float tiling = max(texInfo.b * 64.0, 0.0625);
        float scale = 1.0 / tiling; // repeats per world unit
        vec3 weights = triplanar_weights(worldN);
        float geomSlope = worldN.y; // up-facing-ness (1 flat, 0 vertical), pre normal-map
        vec3 baseAlbedo = triplanar_albedo(fs_in.WorldPos, weights, texLayer, scale);
        vec3 baseN      = triplanar_normal(fs_in.WorldPos, worldN, weights, normLayer, scale);

        // T-I6 macro material variation (RENDER-ONLY, no world_hash): overlay ROCK on
        // steep faces so natural terrain stops reading as one uniform olive material
        // (the BF4/BF1 macro-variation lift). Only natural ground ids (Stone/Soil/Grass
        // = 1..3); calibrated Sand (4) and the flat far-water sheet (200) are untouched,
        // so the sand exposure calibration + the live/far water seam are unchanged. The
        // rock layers are read from the Stone (id 1) material row so this tracks
        // materials.json (no hardcoded layer index). Steep faces blend toward rock with
        // a world-space-noise-jittered boundary so cliffs read as natural scree, not a
        // clean contour line.
        if (fs_in.MaterialID >= 1u && fs_in.MaterialID <= 3u) {
            float jitter = (vnoise(fs_in.WorldPos * 0.05) - 0.5) * 0.18; // ~20 m break-up
            float rockW = smoothstep(0.80 + jitter, 0.50 + jitter, geomSlope); // steep -> rock
            if (rockW > 0.002) {
                vec4 rockInfo = texture(u_materialLUT, vec2(1.0/255.0, 0.5)); // Stone row 1
                float rockTex   = floor(rockInfo.r * 255.0 + 0.5);
                float rockNrm   = floor(rockInfo.g * 255.0 + 0.5);
                float rockScale = 1.0 / max(rockInfo.b * 64.0, 0.0625);
                vec3 rockAlbedo = triplanar_albedo(fs_in.WorldPos, weights, rockTex, rockScale);
                vec3 rockN      = triplanar_normal(fs_in.WorldPos, worldN, weights, rockNrm, rockScale);
                baseAlbedo = mix(baseAlbedo, rockAlbedo, rockW);
                baseN      = normalize(mix(baseN, rockN, rockW));
            }
        }
        worldN = baseN;
        // T-I5b-5-water-backlog: per-material albedo calibration on the textured
        // terrain path (live AND far-LOD sand both sample this triplanar branch -
        // sand carries has_texture). Default scale 1.0 is a no-op (byte-identical)
        // for every unscaled id. Confined to the triplanar branch so flat-material
        // and skinned paths are untouched (no spurious scaling of the case-switch
        // base colors). Brings the noon sun-bright sand flat down to a natural lit
        // tone below the ACES clip.
        albedo = baseAlbedo * albedoScale;
        textured = true;
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

    gAlbedoRoughness = vec4(albedo, roughness);
    gMetallicAO = vec2(metallic, ao);
}
