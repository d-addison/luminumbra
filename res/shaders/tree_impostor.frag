#version 450 core
//  far-field tree impostor fragment. Selects the octahedral atlas tile from the world view
// direction (matching OctaImpostor.h / HemiOctaEncode), samples the albedo + normal atlases, applies
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

uniform sampler2D u_albedo;   // impostor albedo atlas (sRGB-encoded; alpha = silhouette mask)
uniform sampler2D u_normal;   // impostor object-space normal atlas (encoded)
uniform float u_grid;         // tiles per axis
uniform float u_materialId;   // tree material id (0..1, == id/255), for gNormalMaterial.a

// Octahedral normal ENCODE for the G-buffer (must match g_buffer.frag encode_octahedral).
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0); }
vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy: octWrap(n.xy);
}

// Hemi-octahedral view-direction ENCODE for atlas-tile selection (must match OctaImpostor.h HemiOctaEncode).
vec2 hemiOctaEncode(vec3 d) {
    float l1 = abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = vec2(d.x, d.z) / max(l1, 1e-5);
    return vec2(p.x + p.y, p.x - p.y) * 0.5 + 0.5;
}

void main() {
    // Pick the octa tile for this view direction, then sample within it at the billboard UV. A half-texel
    // inset keeps bilinear filtering from bleeding into neighbouring tiles.
    vec2 tuv = clamp(hemiOctaEncode(normalize(vViewDir)), 0.0, 1.0);
    vec2 tile = clamp(floor(tuv * u_grid), vec2(0.0), vec2(u_grid - 1.0));
    float tileRes = float(textureSize(u_albedo, 0).x) / u_grid;
    vec2 uvInTile = clamp(vQuadUV, 0.5 / tileRes, 1.0 - 0.5 / tileRes);
    vec2 atlasUV = (tile + uvInTile) / u_grid;

    vec4 alb = texture(u_albedo, atlasUV);
    if (alb.a < 0.5) discard; // silhouette + leaf-gap cutout

    vec3 nEnc = texture(u_normal, atlasUV).rgb;
    vec3 worldN = nEnc * 2.0 - 1.0;
    if (dot(worldN, worldN) < 1e-4) worldN = vec3(0.0, 1.0, 0.0);
    worldN = normalize(worldN);

    gPosition = vViewPos;
    gNormalMaterial = vec4(encode_octahedral(worldN) * 0.5 + 0.5, 0.0, u_materialId);
    // The impostor atlas is baked sRGB-encoded (ImpostorBake oAlbedo = pow(col,1/2.2))
    // into a plain GL_RGBA8 target, so the sampled value is display-space. The deferred
    // G-buffer albedo is LINEAR (the near-mesh path uses a GL_SRGB8_ALPHA8 array that
    // auto-linearizes), so writing the atlas value verbatim over-brightened far impostor
    // trees ~2x (owner-reported "white filter on trees"). Linearize on read to match.
    gAlbedoRoughness = vec4(pow(alb.rgb, vec3(2.2)), 0.9);   // foliage: rough, non-metallic
    gMetallicAO = vec2(0.0, 1.0);
    gMotionVector = vec2(0.0);
}
