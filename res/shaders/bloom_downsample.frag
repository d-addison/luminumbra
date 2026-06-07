#version 450 core

out vec3 FragColor;

in vec2 TexCoords;

uniform sampler2D u_sourceTexture;
uniform vec2 u_sourceTexelSize;
uniform float u_threshold = 1.0;
uniform float u_thresholdKnee = 0.5;

// Improved bloom threshold with soft knee
vec3 quadraticThreshold(vec3 color, float threshold, float knee) {
    float brightness = max(color.r, max(color.g, color.b));
    
    float soft = brightness - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    
    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 0.00001);
    
    return color * contribution;
}

// 13-tap downsampling with Karis average for better quality
vec3 downsampleBox13Tap(sampler2D tex, vec2 uv, vec2 texelSize) {
    vec3 a = texture(tex, uv + texelSize * vec2(-1.0, -1.0)).rgb;
    vec3 b = texture(tex, uv + texelSize * vec2( 0.0, -1.0)).rgb;
    vec3 c = texture(tex, uv + texelSize * vec2( 1.0, -1.0)).rgb;
    vec3 d = texture(tex, uv + texelSize * vec2(-0.5, -0.5)).rgb;
    vec3 e = texture(tex, uv + texelSize * vec2( 0.5, -0.5)).rgb;
    vec3 f = texture(tex, uv + texelSize * vec2(-1.0,  0.0)).rgb;
    vec3 g = texture(tex, uv                              ).rgb;
    vec3 h = texture(tex, uv + texelSize * vec2( 1.0,  0.0)).rgb;
    vec3 i = texture(tex, uv + texelSize * vec2(-0.5,  0.5)).rgb;
    vec3 j = texture(tex, uv + texelSize * vec2( 0.5,  0.5)).rgb;
    vec3 k = texture(tex, uv + texelSize * vec2(-1.0,  1.0)).rgb;
    vec3 l = texture(tex, uv + texelSize * vec2( 0.0,  1.0)).rgb;
    vec3 m = texture(tex, uv + texelSize * vec2( 1.0,  1.0)).rgb;
    
    // Karis average (prevents fireflies)
    vec3 groups[5];
    groups[0] = (a + b + d + e) * 0.25;
    groups[1] = (b + c + e + j) * 0.25; 
    groups[2] = (d + e + g + i) * 0.25;
    groups[3] = (e + j + g + h) * 0.25;
    groups[4] = (i + j + l + m) * 0.25;
    
    groups[0] *= 1.0 / (1.0 + dot(groups[0], vec3(0.299, 0.587, 0.114)));
    groups[1] *= 1.0 / (1.0 + dot(groups[1], vec3(0.299, 0.587, 0.114)));
    groups[2] *= 1.0 / (1.0 + dot(groups[2], vec3(0.299, 0.587, 0.114)));
    groups[3] *= 1.0 / (1.0 + dot(groups[3], vec3(0.299, 0.587, 0.114)));
    groups[4] *= 1.0 / (1.0 + dot(groups[4], vec3(0.299, 0.587, 0.114)));
    
    return (groups[0] + groups[1] + groups[2] + groups[3] + groups[4]) * 0.2;
}

void main() {
    vec3 color = downsampleBox13Tap(u_sourceTexture, TexCoords, u_sourceTexelSize);
    
    // Apply threshold on first downsample pass
    if (u_threshold > 0.0) {
        color = quadraticThreshold(color, u_threshold, u_thresholdKnee);
    }
    
    FragColor = color;
}