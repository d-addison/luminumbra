#version 450 core
//  TAAU resolve: temporal anti-aliasing (and the hook for upsampling). Blends the current
// jittered HDR frame with the motion-reprojected history, rejecting ghosting via a 3x3
// neighborhood AABB clamp of the history toward the current color box. Halton sub-pixel jitter on
// the G-buffer projection (applied in RenderPipeline) supplies the sub-sample variation that the
// history accumulation turns into anti-aliasing. Flag-gated (render.taau); OFF -> this pass never
// runs and the lighting color blits straight through (byte-identical default).
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D u_current;   // this frame's lit HDR color (RGBA16F)
uniform sampler2D u_history;   // previous resolved color (RGBA16F)
uniform sampler2D u_motion;    // RG16F screen motion vector (NDC delta = curr - prev), G-buffer
uniform vec2  u_texel;         // 1 / screen size
uniform float u_blend;         // history weight when stable (e.g. 0.9); first frame forced to 0
uniform float u_sharpness;     // post-resolve unsharp strength (recovers temporal-blur softness)
uniform int   u_history_valid; // 0 on the first frame / after a resize -> use current only

void main() {
    vec3 curr = texture(u_current, TexCoords).rgb;

    if (u_history_valid == 0) {
        FragColor = vec4(curr, 1.0);
        return;
    }

    // Reproject: previous UV = current UV - motionNDC * 0.5 (uv = ndc*0.5+0.5, so duv = dndc*0.5).
    vec2 motion = texture(u_motion, TexCoords).rg;
    vec2 histUV = TexCoords - motion * 0.5;

    // Disoccluded / off-screen history: no reliable sample -> keep the current pixel (no ghosting).
    if (histUV.x < 0.0 || histUV.x > 1.0 || histUV.y < 0.0 || histUV.y > 1.0) {
        FragColor = vec4(curr, 1.0);
        return;
    }

    vec3 hist = texture(u_history, histUV).rgb;

    // Anti-ghosting: clamp the (possibly stale) history into the 3x3 current-color AABB so a
    // surface that changed shading/occlusion this frame cannot drag an old color forward.
    vec3 nmin = curr;
    vec3 nmax = curr;
    vec3 nsum = curr;  // 3x3 sum (incl. centre) -> mean, for the unsharp sharpen below
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            vec3 c = texture(u_current, TexCoords + vec2(float(dx), float(dy)) * u_texel).rgb;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
            nsum += c;
        }
    }
    hist = clamp(hist, nmin, nmax);

    // Blend more toward history where motion is small (stable -> accumulate AA); fall toward the
    // current frame under fast motion so reprojection error / smearing stays bounded.
    float speed = length(motion);
    float blend = u_blend * (1.0 - clamp(speed * 8.0, 0.0, 1.0) * 0.6);
    vec3 resolved = mix(curr, hist, blend);

    // Unsharp sharpen: TAA's history blend + bilinear reprojection soften the image; pull the result
    // away from the 3x3 current-color mean to recover crispness (clamped non-negative for HDR).
    vec3 mean = nsum * (1.0 / 9.0);
    resolved = max(resolved + u_sharpness * (resolved - mean), vec3(0.0));
    FragColor = vec4(resolved, 1.0);
}
