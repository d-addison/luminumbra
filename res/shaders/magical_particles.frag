#version 450 core

// ===========================================================================
// T-I5a-1: GPU particle framework fragment stage.
//
// Forward-lit (sun + ambient + nearest point lights) emissive billboard with
// soft-particle alpha fade against the scene depth. Blended into the lit HDR
// (RGBA16F) lighting target by ParticlePass.
//
// The input interface block (VS_OUT) is the SAME block written by both
// magical_particles.vert (live vert->frag link) and magical_particles.geom
// (inventory vert->geom->frag link), so the fragment stage links in both.
// ===========================================================================

out vec4 FragColor;

in VS_OUT {
    vec2  texCoord;
    vec4  color;
    flat float atlasLayer;
    float distanceToCamera;
    float viewDepth;
    vec3  worldPos;
} fs_in;

uniform float u_time;
uniform vec3  u_cameraPos;
uniform vec2  u_screenSize;
uniform float u_nearPlane;
uniform float u_farPlane;

// Forward lighting.
uniform vec3  u_sunDirection;  // light travel direction
uniform vec3  u_sunColor;
uniform float u_sunIntensity;
uniform vec3  u_ambientColor;

struct PointLight {
    vec3  position;
    vec3  color;
    float radius;
    float intensity;
};
uniform int        u_pointLightCount;
uniform PointLight u_pointLights[4];

// Scene depth for soft-particle fade.
uniform sampler2D u_sceneDepth;

// Linearize a hardware depth-buffer sample to view-space depth (positive).
float linearize_depth(float d) {
    float z = d * 2.0 - 1.0; // NDC
    return (2.0 * u_nearPlane * u_farPlane) /
           (u_farPlane + u_nearPlane - z * (u_farPlane - u_nearPlane));
}

// Radial soft sprite mask.
float sprite_mask(vec2 uv) {
    float dist = length(uv - vec2(0.5));
    return 1.0 - smoothstep(0.25, 0.5, dist);
}

void main() {
    vec2 uv = fs_in.texCoord;
    float shape = sprite_mask(uv);
    if (shape <= 0.0) {
        discard;
    }

    // --- Soft-particle fade: shrink alpha where the billboard nears opaque
    // geometry, reading the G-buffer depth. ---
    vec2 screenUV = gl_FragCoord.xy / u_screenSize;
    float sceneDepthRaw = texture(u_sceneDepth, screenUV).r;
    float sceneViewDepth = linearize_depth(sceneDepthRaw);
    float softFade = clamp((sceneViewDepth - fs_in.viewDepth) / 1.5, 0.0, 1.0);

    // --- Forward lighting (the billboard faces the camera; use the toward-sun
    // half-Lambert plus ambient and nearby point lights). ---
    vec3 normal = normalize(u_cameraPos - fs_in.worldPos);
    float ndl = max(dot(normal, -normalize(u_sunDirection)), 0.0);
    vec3 lit = u_ambientColor + u_sunColor * u_sunIntensity * (0.5 + 0.5 * ndl);

    for (int i = 0; i < u_pointLightCount; ++i) {
        vec3 toLight = u_pointLights[i].position - fs_in.worldPos;
        float d = length(toLight);
        float atten = clamp(1.0 - d / max(u_pointLights[i].radius, 0.001), 0.0, 1.0);
        atten *= atten;
        lit += u_pointLights[i].color * u_pointLights[i].intensity * atten;
    }

    vec4 finalColor = fs_in.color;
    // Emissive core: the particle is its own light source, modulated by the
    // forward-lit term so it still reads the scene's mood.
    finalColor.rgb *= (0.6 + 0.4 * lit);
    finalColor.rgb += fs_in.color.rgb * shape * 0.5; // HDR glow core
    finalColor.a *= shape * softFade;

    if (finalColor.a < 0.01) {
        discard;
    }

    FragColor = finalColor;
}
