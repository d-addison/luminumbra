#version 450 core
// Render-optimization / fidelity (reference-loop, storm_moor): screen-space
// crepuscular rays (god rays). Radially marches the post-sky scene toward the sun's
// screen position, accumulating BRIGHT (sky) pixels with exponential decay — where
// occluders (clouds, terrain, hills) block the bright sky, shafts of light fan out
// around them. Composited ADDITIVELY over the lit scene. Render-only; gated off when
// the sun is below the horizon or off-screen (zero contribution). Reads a separate
// scene snapshot (no framebuffer feedback).
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_scene;   // post-sky scene snapshot (display-space)
uniform vec2 u_sunUV;        // sun screen-space position [0,1]
uniform float u_sunVisible;  // 0..1 master gate (above horizon + on screen)
uniform float u_strength;    // overall intensity

const int SAMPLES = 24; // radial blur stays smooth at 24; keeps the pass cheap

void main() {
    if (u_sunVisible <= 0.0) { FragColor = vec4(0.0); return; }
    // March from this pixel toward the sun, sampling the scene along the ray.
    vec2 delta = (u_sunUV - TexCoords) / float(SAMPLES) * 0.9;
    vec2 uv = TexCoords;
    float w = 0.5;
    const float decay = 0.955;
    vec3 accum = vec3(0.0);
    for (int i = 0; i < SAMPLES; ++i) {
        uv += delta;
        vec3 s = texture(u_scene, clamp(uv, 0.0, 1.0)).rgb;
        // Only bright sky contributes; dark occluders leave gaps -> shafts.
        float lum = dot(s, vec3(0.2126, 0.7152, 0.0722));
        float bright = smoothstep(0.5, 0.95, lum);
        accum += s * bright * w;
        w *= decay;
    }
    accum /= float(SAMPLES);
    // Radial falloff so the shafts concentrate around the sun, fade across frame.
    float radial = 1.0 - smoothstep(0.0, 1.1, length(u_sunUV - TexCoords));
    FragColor = vec4(accum * u_sunVisible * u_strength * radial, 1.0);
}
