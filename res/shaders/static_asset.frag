#version 450 core
layout(location = 0) out vec3 gPosition;
layout(location = 1) out vec4 gNormalMaterial;
layout(location = 2) out vec4 gAlbedoRoughness;
layout(location = 3) out vec2 gMetallicAO;
layout(location = 4) out vec2 gMotionVector;
layout(location = 5) out vec4 gAuthoredSurface;
in vec3 worldPosition;
in vec3 worldNormal;
in vec3 viewPosition;
in vec2 rawUv;
uniform sampler2D u_baseColor;
uniform sampler2D u_metallicRoughness;
uniform sampler2D u_normalMap;
uniform sampler2D u_occlusion;
uniform sampler2D u_emissive;
uniform mat3 u_uv[5];
uniform mat3 u_normalView;
uniform vec4 u_baseFactor;
uniform vec3 u_emissiveFactor;
uniform float u_metallicFactor;
uniform float u_roughnessFactor;
uniform float u_normalScale;
uniform float u_occlusionStrength;
uniform float u_alphaCutoff;
uniform int u_mask;
uniform int u_doubleSided;
uniform int u_hasNormal;
vec2 sampleUv(int index) { return (u_uv[index] * vec3(rawUv, 1.0)).xy; }
vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 signNotZero = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * signNotZero;
}
void main() {
    vec4 base = texture(u_baseColor, sampleUv(0)) * u_baseFactor;
    if (u_mask != 0 && base.a < u_alphaCutoff) discard;
    vec3 mr = texture(u_metallicRoughness, sampleUv(1)).rgb;
    vec3 normal = normalize(worldNormal);
    // Derive tangent orientation from the compiler-selected RAW UVs. Texture
    // offset/rotation/scale only controls sampling, never the surface tangent basis.
    if (u_hasNormal != 0) {
        vec3 tangentNormal = texture(u_normalMap, sampleUv(2)).xyz * 2.0 - 1.0;
        tangentNormal.xy *= u_normalScale;
        vec3 dp1 = dFdx(worldPosition), dp2 = dFdy(worldPosition);
        vec2 duv1 = dFdx(rawUv), duv2 = dFdy(rawUv);
        vec3 p2 = cross(dp2, normal), p1 = cross(normal, dp1);
        vec3 tangent = p2 * duv1.x + p1 * duv2.x;
        vec3 bitangent = p2 * duv1.y + p1 * duv2.y;
        float extent = max(dot(tangent, tangent), dot(bitangent, bitangent));
        if (extent > 1e-12) {
            float scale = inversesqrt(extent);
            vec3 mapped = tangent * tangentNormal.x * scale
                        + bitangent * tangentNormal.y * scale + normal * tangentNormal.z;
            if (dot(mapped, mapped) > 1e-12) normal = normalize(mapped);
        }
    }
    if (u_doubleSided != 0 && !gl_FrontFacing) normal = -normal;
    gPosition = viewPosition;
    gNormalMaterial = vec4(octahedral(normalize(u_normalView * normal)) * .5 + .5, 0.0, 0.0);
    gAlbedoRoughness = vec4(base.rgb, mr.g * u_roughnessFactor);
    float ao = mix(1.0, texture(u_occlusion, sampleUv(3)).r, u_occlusionStrength);
    gMetallicAO = vec2(mr.b * u_metallicFactor, ao);
    // The first static profile has no temporal history; its caller refuses TAAU.
    gMotionVector = vec2(0.0);
    gAuthoredSurface = vec4(texture(u_emissive, sampleUv(4)).rgb * u_emissiveFactor, 1.0);
}
