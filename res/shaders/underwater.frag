#version 410 core
out vec4 FragColor;

in vec2 v_TexCoords;

uniform sampler2D u_sceneTexture;
uniform sampler2D u_depthTexture;
uniform sampler2D u_dudvMap;

uniform float u_time;
uniform float u_farPlane;
uniform vec3 u_fogColor;
uniform float u_fogDensity;

void main() {
    // 1. Subtle wavering distortion using a DUDV map
    vec2 distortion = (texture(u_dudvMap, v_TexCoords + u_time * 0.01).rg * 2.0 - 1.0) * 0.003;
    vec2 distortedCoords = v_TexCoords + distortion;
    
    vec4 sceneColor = texture(u_sceneTexture, distortedCoords);
    float depth = texture(u_depthTexture, distortedCoords).r;

    // 2. Murky Fog using linearized depth and uniforms from C++
    // This is more physically plausible and controllable than the previous method.
    float linearDepth = depth * u_farPlane;
    float fogFactor = 1.0 - exp(-linearDepth * u_fogDensity);
    fogFactor = clamp(fogFactor, 0.0, 0.95); // Clamp to prevent full whiteout

    vec3 color = mix(sceneColor.rgb, u_fogColor, fogFactor);

    FragColor = vec4(color, 1.0);
}