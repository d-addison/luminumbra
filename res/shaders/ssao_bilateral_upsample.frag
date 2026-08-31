#version 450 core
// Render-optimization (ssao-gtao  / halfres-upsample-infra): joint-bilateral
// depth-aware upsample of the half-resolution GTAO buffer back to full screen,
// replacing the box blur on the half-res path. The AO is rendered at 1/2 per axis
// (1/4 the fragments -> the horizon march cost drops ~4x); this reconstructs the
// full-res AO using the FULL-RES view-space depth (gPosition.z) as the edge guide,
// so the AO does NOT bleed across depth discontinuities (thin geometry: trunks,
// foliage, silhouettes). Falls back to the nearest tap where all weights collapse.
// Also serves as the spatial denoise for the low-tap GTAO. Render-only.
out float FragColor;

in vec2 TexCoords;

uniform sampler2D u_aoHalf;    // half-res AO (R16F)
uniform sampler2D gPosition;   // full-res view-space position (z = depth guide)
uniform vec2 u_halfTexel;      // 1.0 / halfResSize

void main() {
    float centerZ = texture(gPosition, TexCoords).z;
    // 3x3 bilateral kernel over the half-res AO, weighted by depth similarity to the
    // full-res center depth. The kernel also smooths the low-spp GTAO noise.
    float aoSum = 0.0;
    float wSum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * u_halfTexel;
            float a = texture(u_aoHalf, uv).r;
            float z = texture(gPosition, uv).z;
            // Depth-similarity weight (view-space metres) + mild spatial falloff.
            float wz = exp(-abs(centerZ - z) * 3.0);
            float ws = (x == 0 && y == 0) ? 1.0: 0.6;
            float w = wz * ws;
            aoSum += a * w;
            wSum += w;
        }
    }
    FragColor = (wSum > 1e-4) ? (aoSum / wSum): texture(u_aoHalf, TexCoords).r;
}
