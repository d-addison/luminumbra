#version 450 core

// ===========================================================================
// T-I5b-1 (F1): instanced foliage scatter fragment stage.
//
// Forward-lit ground cover blended into the lit HDR target after the opaque
// terrain. A simple blade-shaped alpha mask + sun/ambient lambert keeps the
// scatter cheap; the distance fade arrives as vs_out.fade from the vertex
// stage. RENDER-ONLY (no sim writes).
// ===========================================================================

in VS_OUT {
    vec2  texCoord;
    vec4  color;
    float fade;
    vec3  worldPos;
    vec3  worldNormal;
} fs_in;

uniform vec3  u_sunDirection;  // direction the sunlight travels
uniform vec3  u_sunColor;
uniform float u_sunIntensity;
uniform vec3  u_ambientColor;

out vec4 FragColor;

void main() {
    // Blade alpha mask: taper toward the tip and soften the vertical edges so
    // the card reads as a tuft rather than a hard quad. texCoord.y: 0 base, 1 tip.
    float edge = 1.0 - abs(fs_in.texCoord.x * 2.0 - 1.0); // 0 at sides, 1 centre
    float taper = mix(1.0, 0.25, fs_in.texCoord.y);       // narrower at the tip
    float alpha = smoothstep(0.0, 0.35, edge * taper) * fs_in.fade;
    if (alpha < 0.02) {
        discard;
    }

    vec3 N = normalize(fs_in.worldNormal);
    float ndl = max(dot(N, -normalize(u_sunDirection)), 0.0);
    vec3 lit = fs_in.color.rgb * (u_ambientColor + u_sunColor * (u_sunIntensity * ndl));

    FragColor = vec4(lit, alpha);
}
