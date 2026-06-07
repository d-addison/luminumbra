#version 450 core

out vec3 FragColor;

in vec2 TexCoords;

uniform sampler2D u_sourceTexture;
uniform vec2 u_sourceTexelSize;
uniform float u_filterRadius = 1.0;
uniform float u_intensity = 1.0;

// High quality tent filter upsampling
vec3 upsampleTent(sampler2D tex, vec2 uv, vec2 texelSize, float radius) {
    vec4 d = texelSize.xyxy * vec4(1.0, 1.0, -1.0, 0.0) * radius;
    
    vec3 s;
    s  = texture(tex, uv - d.xy).rgb;
    s += texture(tex, uv - d.wy).rgb * 2.0;
    s += texture(tex, uv - d.zy).rgb;
    
    s += texture(tex, uv + d.zw).rgb * 2.0;
    s += texture(tex, uv       ).rgb * 4.0;
    s += texture(tex, uv + d.xw).rgb * 2.0;
    
    s += texture(tex, uv + d.zy).rgb;
    s += texture(tex, uv + d.wy).rgb * 2.0;
    s += texture(tex, uv + d.xy).rgb;
    
    return s * (1.0 / 16.0);
}

void main() {
    vec3 bloom = upsampleTent(u_sourceTexture, TexCoords, u_sourceTexelSize, u_filterRadius);
    FragColor = bloom * u_intensity;
}