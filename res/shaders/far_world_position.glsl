// Mirrors FarWorldDepthPosition.h. The Legacy branch retains the texture read.
uniform int u_farWorldLadder = 0;
uniform mat4 u_gbufferInverseProjection;
const float FAR_WORLD_POSITION_THRESHOLD = 2000.0;

vec2 farWorldPositionSampleUv(vec2 uv, vec2 textureSizePixels) {
    return (floor(uv * textureSizePixels) + 0.5) / textureSizePixels;
}

bool reconstructFarWorldDepthPosition(vec2 uv, float depth, mat4 inverseProjection,
                                     inout vec3 position) {
    if (isnan(depth) || isinf(depth) || depth <= 0.0 || depth > 1.0) return false;
    vec4 q = inverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    if (any(isnan(q)) || any(isinf(q)) || abs(q.w) <= 1e-8) return false;
    vec3 p = q.xyz / q.w;
    float radialDistance = length(p);
    if (isnan(radialDistance) || isinf(radialDistance) ||
        radialDistance <= 1e-6 || p.z >= 0.0) return false;
    position = p;
    return true;
}

bool farWorldPosition(sampler2D storedPosition, sampler2D floatDepth, vec2 uv,
                      inout vec3 position) {
    if (u_farWorldLadder == 0) {
        position = texture(storedPosition, uv).rgb;
        return true;
    }
    vec3 reconstructed = vec3(0.0);
    vec2 sampleUv = farWorldPositionSampleUv(uv, vec2(textureSize(floatDepth, 0)));
    if (!reconstructFarWorldDepthPosition(sampleUv, texture(floatDepth, uv).r,
                                         u_gbufferInverseProjection, reconstructed)) return false;
    // Classify using float depth, never the quantized RGB16F position.
    if (length(reconstructed) > FAR_WORLD_POSITION_THRESHOLD) {
        position = reconstructed;
        return true;
    }
    vec3 stored = texture(storedPosition, uv).rgb;
    if (any(isnan(stored)) || any(isinf(stored))) return false;
    position = stored;
    return true;
}
