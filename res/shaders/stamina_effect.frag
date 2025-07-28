#version 410 core
out vec4 FragColor;

in vec2 v_TexCoords;

uniform sampler2D u_screenTexture;
uniform float u_time;
uniform float u_staminaRatio;

const float ABERRATION_STRENGTH = 0.05;

void main() {
    // Effect smoothly fades in as stamina drops below 50%
    float effectIntensity = 1.0 - smoothstep(0.0, 0.5, u_staminaRatio);

    if (effectIntensity <= 0.01) {
        FragColor = texture(u_screenTexture, v_TexCoords);
        return;
    }

    vec2 uv_centered = v_TexCoords - 0.5;

    // 1. Chromatic Aberration
    float offset = length(uv_centered) * effectIntensity * ABERRATION_STRENGTH;
    float r = texture(u_screenTexture, v_TexCoords - offset).r;
    float g = texture(u_screenTexture, v_TexCoords).g;
    float b = texture(u_screenTexture, v_TexCoords + offset).b;
    vec3 color = vec3(r, g, b);

    // 2. Desaturation
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    vec3 grayscale = vec3(luminance);
    color = mix(color, grayscale, effectIntensity);

    // 3. Pulsing Vignette
    float pulse = 1.0 + sin(u_time * 8.0) * 0.05 * effectIntensity;
    float vignette = 1.0 - (dot(uv_centered, uv_centered) * pulse * (effectIntensity * 1.5));
    vignette = clamp(vignette, 0.0, 1.0);
    
    color *= vignette;
    
    FragColor = vec4(color, 1.0);
}