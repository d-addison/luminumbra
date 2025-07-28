#version 410
out vec4 FragColor;

in vec2 TexCoords;
in vec3 WorldPos;
in vec4 Color;
in float Layer;

uniform float u_Time;

float hash(float n) { return fract(sin(n) * 43758.5453123); }

float noise(vec3 x) {
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    
    float n = p.x + p.y * 157.0 + 113.0 * p.z;
    return mix(
        mix(mix(hash(n + 0.0), hash(n + 1.0), f.x),
            mix(hash(n + 157.0), hash(n + 158.0), f.x), f.y),
        mix(mix(hash(n + 113.0), hash(n + 114.0), f.x),
            mix(hash(n + 270.0), hash(n + 271.0), f.x), f.y), f.z);
}

float fbm(vec3 x) {
    float v = 0.0;
    float a = 0.5;
    vec3 shift = vec3(100);
    
    for (int i = 0; i < 4; ++i) {
        v += a * noise(x);
        x = x * 2.0 + shift;
        a *= 0.5;
    }
    return v;
}

void main() {
    // Base shape
    float dist = length(TexCoords - vec2(0.5));
    float alpha = smoothstep(0.5, 0.2, dist);
    
    // Add 3D noise
    vec3 noiseCoord = WorldPos * 0.5 + vec3(u_Time * 0.1);
    float noiseVal = fbm(noiseCoord);
    
    // Layer fade
    float layerFade = 1.0 - (Layer * 0.1);
    
    // Combine
    alpha *= noiseVal * layerFade;
    alpha = smoothstep(0.1, 0.6, alpha);
    
    FragColor = vec4(Color.rgb, Color.a * alpha);
}