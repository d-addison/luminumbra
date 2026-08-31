#version 450 core
// Render-optimization (ssao-gtao, ): Ground-Truth Ambient Occlusion
// (Jimenez et al. 2016 / Intel XeGTAO), horizon-slice formulation, replacing the
// 64-sample hemisphere SSAO (ssao.frag). Quality comes from the horizon arc
// integral + a spatio-temporal denoise, NOT raw tap count: High = 3 slices x 6
// steps = 18 spp (vs 64), cheaper AND ground-truth-closer. Reads the SAME
// view-space G-buffer (gPosition + octahedral gNormal), so it drops into the
// existing SSAO FBO + blur path.: no world_hash impact. Selected at
// runtime by render ssao_quality (legacy ssao.frag is the byte-identical default).
out float FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;       // view-space position (xyz)
uniform sampler2D gNormalMaterial; // octahedral view-space normal in.xy
uniform mat4  u_projection;        // for the world->pixel radius (proj[1][1])
uniform vec2  u_screenSize;
uniform float u_radius   = 0.8;    // view-space AO radius (metres), matches legacy RADIUS
uniform int   u_sliceCount = 3;    // High = 3
uniform int   u_stepsPerSlice = 6; // High = 6  (-> 18 spp)
uniform float u_frame = 0.0;       // temporal rotation seed (0 = spatial-only)

const float PI = 3.14159265359;
const float HALF_PI = 1.57079632679;

vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}
vec3 decode_octahedral(vec2 encoded) {
    vec2 e = encoded * 2.0 - 1.0;
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = octWrap(v.xy);
    return normalize(v);
}

// Interleaved gradient noise (Jimenez) for the per-pixel slice rotation + step
// jitter — interleaved, NOT per-pixel random (cache coherency, per the research).
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec3 P = texture(gPosition, TexCoords).rgb;
    // Background / sky (no geometry written) -> fully unoccluded. View space looks
    // down -z, so valid geometry has P.z < 0; a (near-)zero position is the cleared
    // background.
    if (-P.z < 1e-4) { FragColor = 1.0; return; }

    vec3 N = decode_octahedral(texture(gNormalMaterial, TexCoords).xy);
    vec3 V = normalize(-P); // toward the camera

    // World/view AO radius -> screen-space pixel radius at this depth.
    float radiusPixels = u_radius * u_projection[1][1] * (0.5 * u_screenSize.y) / max(1e-3, -P.z);
    radiusPixels = clamp(radiusPixels, 2.0, 256.0);
    vec2 texel = 1.0 / u_screenSize;

    float noise = ign(gl_FragCoord.xy + u_frame * 7.0);

    float occlusion = 0.0;
    float weightSum = 0.0;

    for (int s = 0; s < u_sliceCount; ++s) {
        float phi = (float(s) + noise) * PI / float(u_sliceCount);
        vec2 omega = vec2(cos(phi), sin(phi)); // screen-space slice direction

        // Slice frame (screen-space dir approximated as a view-space tangent — the
        // standard screen-space GTAO approximation; the horizons themselves are
        // exact, computed from real view-space sample deltas below).
        vec3 sliceDir = vec3(omega, 0.0);
        vec3 axis = normalize(cross(sliceDir, V));
        vec3 projN = N - axis * dot(N, axis);
        float projNLen = length(projN);
        if (projNLen < 1e-4) continue;
        vec3 projNn = projN / projNLen;
        float n = acos(clamp(dot(projNn, V), -1.0, 1.0)) * sign(dot(projN, sliceDir));

        // Horizon search: closest occluder cos-angle to V on each side.
        float cH1 = -1.0; // +omega side
        float cH2 = -1.0; // -omega side
        for (int k = 1; k <= u_stepsPerSlice; ++k) {
            float t = (float(k) - 0.5 + noise) / float(u_stepsPerSlice);
            float px = t * radiusPixels;

            vec3 d1 = texture(gPosition, TexCoords + omega * px * texel).rgb - P;
            float len1 = length(d1);
            if (len1 > 1e-4 && -(d1.z + P.z) < 1e9) {
                float falloff = clamp(1.0 - len1 / u_radius, 0.0, 1.0);
                float c = dot(d1 / len1, V);
                cH1 = max(cH1, mix(-1.0, c, falloff));
            }
            vec3 d2 = texture(gPosition, TexCoords - omega * px * texel).rgb - P;
            float len2 = length(d2);
            if (len2 > 1e-4) {
                float falloff = clamp(1.0 - len2 / u_radius, 0.0, 1.0);
                float c = dot(d2 / len2, V);
                cH2 = max(cH2, mix(-1.0, c, falloff));
            }
        }

        // Signed horizon angles about V, clamped to the hemisphere around n.
        float H1 = n + min( acos(clamp(cH1, -1.0, 1.0)) - n,  HALF_PI);
        float H2 = n + max(-acos(clamp(cH2, -1.0, 1.0)) - n, -HALF_PI);

        // GTAO arc inner-integral (cosine-weighted visibility over the slice).
        float aoSlice = 0.25 * ((-cos(2.0 * H1 - n) + cos(n) + 2.0 * H1 * sin(n))
                              + (-cos(2.0 * H2 - n) + cos(n) + 2.0 * H2 * sin(n)));
        occlusion += projNLen * aoSlice;
        weightSum += projNLen;
    }

    float visibility = (weightSum > 1e-4) ? (occlusion / weightSum): 1.0;
    visibility = clamp(visibility, 0.0, 1.0);
    // Match the legacy contrast curve so the lighting pass AO response is familiar.
    FragColor = pow(visibility, 2.2);
}
