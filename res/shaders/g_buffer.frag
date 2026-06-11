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

// Triplanar blend weights from a world-space normal (sharpened, normalized).
vec3 triplanar_weights(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));
    return w / max(w.x + w.y + w.z, 1e-4);
}

// Triplanar albedo sample from the terrain array.
vec3 triplanar_albedo(vec3 worldPos, vec3 weights, float layer, float scale) {
    vec2 uv_x = worldPos.zy * scale;
    vec2 uv_y = worldPos.xz * scale;
    vec2 uv_z = worldPos.xy * scale;
    vec3 cx = texture(u_terrainTextures, vec3(uv_x, layer)).rgb;
    vec3 cy = texture(u_terrainTextures, vec3(uv_y, layer)).rgb;
    vec3 cz = texture(u_terrainTextures, vec3(uv_z, layer)).rgb;
    return cx * weights.x + cy * weights.y + cz * weights.z;
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
        albedo = triplanar_albedo(fs_in.WorldPos, weights, texLayer, scale);
        worldN = triplanar_normal(fs_in.WorldPos, worldN, weights, normLayer, scale);
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
