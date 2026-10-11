#version 450 core
//  far-field tree impostor fragment. Selects the octahedral atlas tile from the world view
// direction (matching OctaImpostor.h / OctaEncode), samples the albedo + normal atlases, applies
// the silhouette cutout (atlas alpha), and writes the SAME deferred G-buffer attachments as
// g_buffer.frag so the impostor lights + depth-sorts exactly like the real tree geometry it replaces.

layout (location = 0) out vec3 gPosition;        // RGB16F: view-space position
layout (location = 1) out vec4 gNormalMaterial;  // RGB10A2: octahedral normal + material id
layout (location = 2) out vec4 gAlbedoRoughness; // RGBA8: albedo + roughness
layout (location = 3) out vec2 gMetallicAO;      // RG16F: metallic + AO
layout (location = 4) out vec2 gMotionVector;    // RG16F: screen motion (0 for static far trees)

in vec2 vQuadUV;
in vec3 vViewDir;
in vec3 vWorldPos;
in vec3 vViewPos;
flat in mat3 vObjectToWorld;
flat in vec3 vTint;
uniform mat4 u_view;

uniform sampler2D u_albedo;   // impostor albedo atlas (linear; alpha = silhouette mask)
uniform sampler2D u_normal;   // octahedral object normal (RG), roughness (B), AO (A)
uniform float u_grid;         // tiles per axis
uniform float u_materialId;   // tree material id (0..1, == id/255), for gNormalMaterial.a

// Octahedral normal ENCODE for the G-buffer (must match g_buffer.frag encode_octahedral).
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0); }
vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy: octWrap(n.xy);
}

// Full-sphere view direction, matching OctaImpostor.h.
vec2 octaEncode(vec3 d) {
    float l1 = abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = vec2(d.x, d.z) / max(l1, 1e-5);
    if (d.y < 0.0) p = (1.0 - abs(p.yx)) * (step(0.0, p.xy) * 2.0 - 1.0);
    return p * 0.5 + 0.5;
}

void main() {
    // Pick the octa tile for this view direction, then sample within it at the billboard UV. A half-texel
    // inset keeps bilinear filtering from bleeding into neighbouring tiles.
    vec2 tuv = clamp(octaEncode(normalize(vViewDir)), 0.0, 1.0);
    vec2 tile = clamp(floor(tuv * u_grid), vec2(0.0), vec2(u_grid - 1.0));
    float tileRes = float(textureSize(u_albedo, 0).x) / u_grid;
    vec2 uvInTile = clamp(vQuadUV, 0.5 / tileRes, 1.0 - 0.5 / tileRes);
    vec2 atlasUV = (tile + uvInTile) / u_grid;

    vec4 alb = texture(u_albedo, atlasUV);
    if (alb.a < 0.5) discard; // silhouette + leaf-gap cutout

    // Empty texels are transparent black in both atlases. Divide by coverage
    // before decoding material values so leaf edges acquire no background tint.
    vec4 normalSample = clamp(texture(u_normal, atlasUV) / alb.a, 0.0, 1.0);
    vec2 oct = normalSample.rg * 2.0 - 1.0;
    vec3 worldN = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (worldN.z < 0.0) worldN.xy = octWrap(worldN.xy);
    worldN = normalize(vObjectToWorld * worldN);
    vec3 viewN = normalize(mat3(u_view) * worldN);

    gPosition = vViewPos;
    gNormalMaterial = vec4(encode_octahedral(viewN) * 0.5 + 0.5, 0.0, u_materialId);
    gAlbedoRoughness = vec4((alb.rgb / alb.a) * vTint, normalSample.b);
    gMetallicAO = vec2(0.0, normalSample.a);
    gMotionVector = vec2(0.0);
}
