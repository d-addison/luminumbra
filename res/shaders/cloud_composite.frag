#version 450 core
// Render-optimization (cloud-raymarch-optimization, ): composite the
// half-resolution sky dome (raymarched clouds + scattering + sun/stars) back
// into the full-res lighting FBO. The dome is rendered to a reduced-resolution
// FBO (1/2 or 1/4 each axis) where the expensive cloud raymarch runs at a
// fraction of the fragment count; this pass upsamples it and writes it ONLY to
// the sky pixels, reproducing the legacy GL_GEQUAL sky mask exactly.
//
// The legacy skybox dome draw used GL_GEQUAL against the scene depth blitted
// into the lighting FBO: the dome rasterizes at gl_FragDepth == 0.0, so it only
// survived where the scene depth was also 0.0 (cleared / far == no geometry).
// Here we replicate that mask in-shader: keep the lit geometry (discard) wherever
// the full-res scene depth is closer than the far plane, and write the upsampled
// sky everywhere else. Because the half-res buffer holds VALID sky at every texel
// (the dome fills the whole viewport), a plain bilinear upsample does not bleed
// across the sky/geometry boundary — the boundary is cut sharply by the full-res
// depth test, not by the (smooth, low-frequency) sky colour. Depth-aware
// bilateral upsampling (halfres-upsample-infra) matters for AO, not this case.
//
// Render-only: no world_hash impact. Active only when render cloud quality > full.
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_cloudColor; // half-res sky dome (LINEAR filtered -> bilinear upsample)
uniform sampler2D u_sceneDepth; // full-res G-buffer depth (D32F)

void main() {
    // Only exactly cleared reversed-Z depth is sky; even the most distant
    // positive geometry depth must retain its lit surface color.
    float d = texture(u_sceneDepth, TexCoords).r;
    if (d > 0.0) {
        discard;
    }
    FragColor = vec4(texture(u_cloudColor, TexCoords).rgb, 1.0);
}
