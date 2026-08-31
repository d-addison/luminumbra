#version 450 core
// the WBOIT resolve — the weighted average of the
// accumulated premultiplied glass colors, composited over the lit scene with
// coverage = 1 - reveal (OitModel.h). A zero-accumulation pixel (no glass)
// resolves to coverage 0: untouched.
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_accum;  // rgb = sum(w*a*c), a = sum(w*a)
uniform sampler2D u_reveal; // r = product(1 - a_i)

void main() {
    vec4 accum = texture(u_accum, TexCoords);
    float reveal = texture(u_reveal, TexCoords).r;
    float coverage = 1.0 - clamp(reveal, 0.0, 1.0);
    if (coverage < 1e-5) {
        discard; // no glass on this pixel
    }
    vec3 avg = accum.rgb / max(accum.a, 1e-5);
    FragColor = vec4(avg, coverage);
}
