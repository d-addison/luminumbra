#version 450 core

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

void main() {
    vec3 viewPos  = texture(gPosition, TexCoords).xyz;
    vec4 nrmData  = texture(gNormalMaterial, TexCoords);
    bool isSky    = dot(viewPos, viewPos) < 1e-8; // empty/sky pixel in view space

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
            float ndc = z * 2.0 - 1.0;
            float lin = (2.0 * u_near * u_far) /
                        (u_far + u_near - ndc * (u_far - u_near));
            outColor = vec3(clamp(lin * u_depthScale, 0.0, 1.0));
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
