#version 450 core
#include "far_world_position.glsl"

// G-BUFFER DEBUG VISUALIZER — render-only diagnostic fullscreen pass (DebugViewPass).
// Overrides the final composite with a single-channel view of the DEFERRED G-buffer so
// a human (or a frame-scan) can tell "dark night" from a real lighting/geometry bug.
// Determinism-neutral: reads the G-buffer only, never the sim; gated entirely by the
// host-side u_mode uniform. u_mode == 0 is never drawn (the pass is skipped host-side).
//
// G-buffer formats (must match g_buffer.frag / RenderPipeline GBuffer):
//   gPosition       RGB16F  VIEW-SPACE position (COLOR_ATTACHMENT0)
//   gNormalMaterial RGBA8   octahedral normal XY in .xy, MATERIAL-ID in .a (ATTACHMENT1)
//   gAlbedo         sRGB8   base color (COLOR_ATTACHMENT2)
//   gMaterial       RGBA8   PBR metallic/rough/AO (COLOR_ATTACHMENT3)  [unused here]
//   gDepth          depth   (sampled for the raw hardware-depth fallback)
//
// Modes (DebugViewPass::Mode): 1=Albedo 2=Normal 3=Depth 4=Material 5=Position.

layout(location = 0) out vec4 FragColor;

uniform sampler2D gPosition;        // unit 0: VIEW-SPACE position (RGB16F)
uniform sampler2D gNormalMaterial;  // unit 1: oct-normal.xy + material-id.a (RGBA8)
uniform sampler2D gAlbedo;          // unit 2: base color (sRGB8)
uniform sampler2D gDepth;           // unit 3: hardware depth texture

#ifdef LUMIN_DIAG_VIEWS
layout(binding = 4) uniform usampler2D gIdentity;
layout(binding = 5) uniform usamplerBuffer gDrawMetadata;
uniform int u_identityReady = 0;
uniform int u_inspect = 0;
uniform int u_pixelX = 0;
uniform int u_pixelY = 0;
layout(std430, binding = 7) buffer InspectorResult {
    uvec4 inspectorId;
    vec4 inspectorAlbedo;
    vec4 inspectorNormalDepth;
};
#endif

uniform int   u_mode;               // 1..5; see DebugViewPass::Mode
uniform float u_near = 0.1;         // camera near plane (for hw-depth linearization)
uniform float u_far  = 4000.0;      // camera far plane
uniform float u_depthScale = 0.004; // view-Z -> [0,1] grayscale scale (tuned host-side)
uniform float u_posScale   = 0.002; // view-space position -> [0,1] remap scale

in vec2 TexCoords;

// --- Octahedral normal decode (matches lighting_pass.frag exactly) ---
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}
vec3 decode_octahedral(vec2 encoded) {
    vec2 e = encoded * 2.0 - 1.0;
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = octWrap(v.xy);
    return normalize(v);
}

// Cheap integer hash -> distinct, well-spread RGB. Used to colorize material ids so
// adjacent ids (e.g. 12 vs 13) are visually far apart.
vec3 hash_id_color(uint id) {
    if (id == 0u) return vec3(0.04); // 0 = "no material" / sky -> near-black
    uint h = id * 2654435761u;       // Knuth multiplicative hash
    float r = float((h >>  0) & 255u) / 255.0;
    float g = float((h >>  8) & 255u) / 255.0;
    float b = float((h >> 16) & 255u) / 255.0;
    // Lift into a readable band (avoid muddy near-black for low entropy ids).
    return clamp(vec3(r, g, b) * 0.85 + 0.15, 0.0, 1.0);
}

#ifdef LUMIN_DIAG_VIEWS
// Stable uint32 hash, identical to DebugViewPass::hash_id.
uint diagnosticHash(uint value) {
    return value * 2654435761u;
}
vec3 diagnosticColor(uint value) {
    uint hashed = diagnosticHash(value);
    return vec3(hashed & 255u, (hashed >> 8u) & 255u, (hashed >> 16u) & 255u) / 255.0;
}
vec3 unsupportedColor() {
    // Legend: magenta hatch = zero, unresolved, unsupported or unavailable.
    return (int(gl_FragCoord.x + gl_FragCoord.y) / 6) % 2 == 0
        ? vec3(1.0, 0.0, 1.0) : vec3(0.15, 0.0, 0.15);
}
vec3 identityColor() {
    if (u_identityReady == 0) return unsupportedColor();
    ivec2 extent = textureSize(gIdentity, 0);
    ivec2 pixel = clamp(ivec2(TexCoords * vec2(extent)), ivec2(0), extent - 1);
    uvec4 id = texelFetch(gIdentity, pixel, 0);
    if (id.x == 0u || id.x >= uint(textureSize(gDrawMetadata) / 2)) return unsupportedColor();
    uvec4 exactId = texelFetch(gDrawMetadata, int(id.x * 2u));
    uvec4 metadata = texelFetch(gDrawMetadata, int(id.x * 2u + 1u));
    if (any(notEqual(id, exactId)) || metadata.y == 0u) return unsupportedColor();
    if (u_mode == 6) return diagnosticColor(id.x);
    if (u_mode == 7) return diagnosticColor(id.y);
    if (u_mode == 8) return diagnosticColor(id.z);
    if (u_mode == 9) return diagnosticColor(metadata.x);
    if (u_mode == 10) {
        // Terrain, impostor, foliage, static, far volume. Owner: live / legacy / ladder.
        const vec3 colors[5] = vec3[5](vec3(0.7,0.4,0.1), vec3(0.1,0.8,0.9),
            vec3(0.2,0.8,0.2), vec3(0.9,0.8,0.2), vec3(0.3,0.3,1.0));
        if (metadata.y > 5u) return unsupportedColor();
        uint owner = id.w & 255u;
        return colors[metadata.y - 1u] * (owner == 1u ? 1.0 : owner == 2u ? 0.7 : 0.45);
    }
    // Mesh path provenance: mesh=hashed path index, SDF=orange, heightfield=blue.
    if (metadata.z == 1u) return diagnosticColor(id.y);
    if (metadata.z == 2u) return vec3(1.0, 0.4, 0.05);
    if (metadata.z == 3u) return vec3(0.1, 0.4, 1.0);
    return unsupportedColor();
}

#endif

void main() {
#ifdef LUMIN_DIAG_VIEWS
    if (u_inspect != 0) {
        ivec2 pixel = ivec2(u_pixelX, u_pixelY);
        inspectorId = texelFetch(gIdentity, pixel, 0);
        vec4 normalMaterial = texelFetch(gNormalMaterial, pixel, 0);
        inspectorAlbedo = vec4(texelFetch(gAlbedo, pixel, 0).rgb,
                               round(normalMaterial.a * 255.0));
        inspectorNormalDepth = vec4(decode_octahedral(normalMaterial.xy),
                                    texelFetch(gDepth, pixel, 0).r);
        FragColor = vec4(0.0);
        return;
    }
    if (u_mode >= 6 && u_mode <= 11) {
        FragColor = vec4(identityColor(), 1.0);
        return;
    }
#endif
    vec3 viewPos = vec3(0.0);
    bool validPosition = farWorldPosition(gPosition, gDepth, TexCoords, viewPos);
    vec4 nrmData  = texture(gNormalMaterial, TexCoords);
    bool isSky    = !validPosition || dot(viewPos, viewPos) < 1e-8; // empty/sky pixel in view space

    vec3 outColor = vec3(0.0);

    if (u_mode == 1) {
        // ALBEDO / base color — UNLIT. Shows geometry + material color with NO lighting,
        // so a black frame here means "no geometry / streaming hole", while a black frame
        // in the normal render with color HERE means "lighting bug", not "dark night".
        outColor = texture(gAlbedo, TexCoords).rgb;
    } else if (u_mode == 2) {
        // VIEW-SPACE NORMAL — decode octahedral .xy, show as 0.5 + 0.5*n. Degenerate /
        // white-triangle geometry shows as flat/uniform or NaN-ish blocks.
        if (isSky) { outColor = vec3(0.0); }
        else {
            vec3 n = decode_octahedral(nrmData.xy);
            outColor = n * 0.5 + 0.5;
        }
    } else if (u_mode == 3) {
        // DEPTH (linearized, grayscale). Prefer the view-space Z (always populated by the
        // gbuffer); fall back to hardware depth for pixels with no view position (sky).
        if (!isSky) {
            float linear = clamp(-viewPos.z * u_depthScale, 0.0, 1.0);
            outColor = vec3(linear);
        } else {
            float z = texture(gDepth, TexCoords).r;          // [0,1] non-linear
            float lin = (u_near * u_far) / (u_near + z * (u_far - u_near));
            if (u_farWorldLadder != 0 && (isnan(z) || isinf(z))) outColor = vec3(0.0);
            else outColor = vec3(clamp(lin * u_depthScale, 0.0, 1.0));
        }
    } else if (u_mode == 4) {
        // MATERIAL-ID — normal_texture.a * 255 hashed to a distinct color for per-layer
        // focus (which material covers which pixel).
        uint matId = uint(round(nrmData.a * 255.0));
        outColor = isSky ? vec3(0.02) : hash_id_color(matId);
    } else if (u_mode == 5) {
        // POSITION — view-space position remapped to [0,1] so spatial structure is visible
        // (gradients across the frame reveal discontinuities / cracks).
        if (isSky) { outColor = vec3(0.0); }
        else { outColor = clamp(viewPos * u_posScale + 0.5, 0.0, 1.0); }
    } else {
        // Defensive: u_mode == 0 should never be drawn (host skips the pass). Magenta
        // makes an accidental draw obvious rather than silently wrong.
        outColor = vec3(1.0, 0.0, 1.0);
    }

    FragColor = vec4(outColor, 1.0);
}
