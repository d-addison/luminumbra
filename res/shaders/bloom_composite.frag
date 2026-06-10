#version 450 core

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_sceneTexture;
uniform sampler2D u_bloomTexture;
uniform float u_bloomStrength = 0.04;
uniform float u_bloomRadius = 1.0;
uniform vec3 u_bloomTint = vec3(1.0, 1.0, 1.0);

// Tone mapping options
uniform int u_tonemapOperator = 0; // 0 = Reinhard, 1 = Filmic, 2 = ACES
uniform float u_exposure = 1.0;
uniform float u_gamma = 2.2;

// Color grading
uniform float u_contrast = 1.0;
uniform float u_saturation = 1.0;
uniform vec3 u_colorFilter = vec3(1.0);

// Lens effects
uniform float u_vignette = 0.2;
uniform float u_filmGrain = 0.1;
uniform float u_time = 0.0;

// Noise for film grain
float random(vec2 st) {
    return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123);
}

// ACES filmic tone mapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Enhanced filmic tone mapping
vec3 filmicTonemap(vec3 color) {
    color = max(vec3(0.0), color - 0.004);
    color = (color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06);
    return pow(color, vec3(2.2));
}

// Reinhard tone mapping
vec3 reinhardTonemap(vec3 color) {
    return color / (1.0 + color);
}

// Apply vignette effect
vec3 applyVignette(vec3 color, vec2 uv, float strength) {
    vec2 center = uv - 0.5;
    float dist = length(center);
    float vignette = 1.0 - smoothstep(0.3, 1.0, dist * strength);
    return color * vignette;
}

// Color grading
vec3 colorGrade(vec3 color, float contrast, float saturation, vec3 color_filter) {
    // Contrast
    color = (color - 0.5) * contrast + 0.5;
    
    // Saturation
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(luminance), color, saturation);
    
    // Color filter
    color *= color_filter;
    
    return color;
}

void main() {
    // Sample scene and bloom
    vec3 sceneColor = texture(u_sceneTexture, TexCoords).rgb;
    vec3 bloomColor = texture(u_bloomTexture, TexCoords).rgb;
    
    // Apply bloom with tint
    bloomColor *= u_bloomTint;
    vec3 hdrColor = sceneColor + bloomColor * u_bloomStrength;
    
    // Exposure adjustment
    hdrColor *= u_exposure;
    
    // Tone mapping
    vec3 ldrColor;
    if (u_tonemapOperator == 0) {
        ldrColor = reinhardTonemap(hdrColor);
    } else if (u_tonemapOperator == 1) {
        ldrColor = filmicTonemap(hdrColor);
    } else {
        ldrColor = ACESFilm(hdrColor);
    }
    
    // Color grading
    ldrColor = colorGrade(ldrColor, u_contrast, u_saturation, u_colorFilter);
    
    // Vignette
    if (u_vignette > 0.0) {
        ldrColor = applyVignette(ldrColor, TexCoords, u_vignette);
    }
    
    // Film grain
    if (u_filmGrain > 0.0) {
        float grain = random(TexCoords + fract(u_time)) * 2.0 - 1.0;
        grain *= u_filmGrain * 0.1;
        ldrColor += vec3(grain);
    }
    
    // Gamma correction
    ldrColor = pow(ldrColor, vec3(1.0 / u_gamma));
    
    // Final clamping
    ldrColor = clamp(ldrColor, 0.0, 1.0);
    
    FragColor = vec4(ldrColor, 1.0);
}
