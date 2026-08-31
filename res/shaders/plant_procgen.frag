#version 450 core
//  render-only PROCEDURAL plant pass (behind render.plant_procgen).
// Writes the SAME deferred G-buffer layout as g_buffer.frag so the downstream
// lighting / SSAO / tonemap chain treats procgen plants exactly like the other
// solid geometry. Self-contained (no material LUT / triplanar): a flat bark vs
// leaf albedo keyed off the bake UV, with a fixed plant material id so the
// lighting pass shades it normally.
layout (location = 0) out vec3 gPosition;        // RGB16F: view-space position
layout (location = 1) out vec4 gNormalMaterial;  // RGBA8: octahedral normal + material id
layout (location = 2) out vec4 gAlbedoRoughness; // RGBA8: albedo + roughness
layout (location = 3) out vec2 gMetallicAO;      // RG16F: metallic + AO

in VS_OUT {
    vec3 FragPos;   // VIEW space
    vec3 Normal;    // VIEW space
    vec2 UV;
    vec3 Color;     // per-vertex albedo (genetic + seasonal)
} fs_in;

// Octahedral normal encoding (identical to g_buffer.frag).
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}
vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

// Flat albedos chosen to read as bark vs foliage under the deferred lighting.
const vec3 kBarkAlbedo = vec3(0.20, 0.12, 0.06);  // brown trunk/branch
const vec3 kLeafAlbedo = vec3(0.16, 0.42, 0.12);  // green canopy
uniform int u_plantMaterialId = 3; // Grass id: shaded as soft vegetation by lighting
// EMISSIVE MARKERS: when this pass draws the gameplay BEACON octahedra (creatures /
// foragers / food / nest / crystals), the C++ sets u_markerEmissive>0 so each marker
// glows in its own (species-tinted) albedo even with no sky/cave light. Stored in the
// otherwise-unused gNormalMaterial.b channel; the lighting pass reads it and adds an
// additive glow scaled DOWN by daylight. Default 0.0 -> .b stays 0.0 -> pixel-identical
// to the pre-change G-buffer for any non-marker use of this shader.
uniform float u_markerEmissive = 0.0;

void main()
{
    // The CPU bake (PlantProcgenPass) OVERWRITES UV.x with a clean per-vertex
    // class flag: 0.0 for woody branch-ring verts, 1.0 for leaf-card verts. So a
    // simple threshold cleanly separates the brown trunk from the green canopy.
    bool isLeaf = fs_in.UV.x >= 0.5;
    // Per-vertex genetic + seasonal albedo from the bake; fall back to the flat constants
    // if a vertex carries no color (length ~0).
    vec3 albedo = dot(fs_in.Color, fs_in.Color) > 1e-6 ? fs_in.Color
                                                       : (isLeaf ? kLeafAlbedo : kBarkAlbedo);
    float roughness = isLeaf ? 0.85 : 0.70;

    gPosition = fs_in.FragPos;
    vec2 encoded_normal = encode_octahedral(normalize(fs_in.Normal));
    float material_id_normalized = float(u_plantMaterialId) / 255.0;
    // Pack the emissive strength into the (previously 0.0) .b channel; the lighting
    // pass multiplies the marker's albedo by this to make it a glowing beacon.
    gNormalMaterial = vec4(encoded_normal * 0.5 + 0.5, clamp(u_markerEmissive, 0.0, 1.0), material_id_normalized);
    gAlbedoRoughness = vec4(albedo, roughness);
    gMetallicAO = vec2(0.0, 1.0);
}
