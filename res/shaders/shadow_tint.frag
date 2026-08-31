#version 450 core
// the tinted-transmission write. Output = the pane's
// Beer-Lambert transmission T(d) = tint^d (GlassTintModel.h — u_tint is the
// unit-thickness transmission), composited into the tint cascade with
// GL_DST_COLOR/GL_ZERO multiply blending so stacked panes accumulate the product.
// Depth-TESTED against the opaque cascade depth (write off): a pane behind an
// opaque occluder contributes nothing (the light never reached it).
out vec4 FragTint;

uniform vec3 u_tint;       // unit-thickness transmission color
uniform float u_thickness; // pane thickness in tint-model units

void main() {
    vec3 t = clamp(u_tint, 0.0, 1.0);
    float d = max(u_thickness, 0.0);
    FragTint = vec4(pow(t, vec3(d)), 1.0);
}
