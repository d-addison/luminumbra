#version 450 core

out vec4 FragColor;

// Input from geometry shader
in GS_OUT {
    vec2 texCoord;
    vec4 color;
    float type;
    float ageFactor;
    vec3 worldPos;
    float distanceToCamera;
} fs_in;

uniform float u_time;
uniform vec3 u_cameraPos;

// Hash function for procedural patterns
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    
    float a = hash(i + vec2(0.0, 0.0));
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Generate different particle shapes procedurally
float generateParticleShape(vec2 uv, float type) {
    vec2 center = uv - vec2(0.5);
    float dist = length(center);
    
    if (type < 0.5) { // Sparkle particles - star shape
        float angle = atan(center.y, center.x);
        float star = 0.3 + 0.2 * cos(angle * 6.0); // 6-pointed star
        float sparkle = smoothstep(star, star - 0.1, dist);
        
        // Add twinkling rays
        float rays = max(
            smoothstep(0.02, 0.0, abs(center.x)) * smoothstep(0.5, 0.0, abs(center.y)),
            smoothstep(0.02, 0.0, abs(center.y)) * smoothstep(0.5, 0.0, abs(center.x))
        );
        
        return max(sparkle, rays * 0.5);
    }
    else if (type < 1.5) { // Ember particles - soft glow with hot center
        float glow = 1.0 - smoothstep(0.0, 0.8, dist);
        float hotCenter = 1.0 - smoothstep(0.0, 0.3, dist);
        
        // Add flickering
        float flicker = noise(vec2(u_time * 5.0 + fs_in.worldPos.x, fs_in.worldPos.y * 0.1));
        flicker = flicker * 0.3 + 0.7;
        
        return (glow + hotCenter * 2.0) * flicker;
    }
    else if (type < 2.5) { // Magic particles - complex magical symbol
        float symbol = 0.0;
        
        // Outer ring
        float outerRing = 1.0 - smoothstep(0.35, 0.45, dist);
        outerRing *= smoothstep(0.25, 0.35, dist);
        symbol = max(symbol, outerRing);
        
        // Inner pentagram
        float angle = atan(center.y, center.x);
        float pentagram = 0.2 + 0.15 * cos(angle * 5.0);
        symbol = max(symbol, smoothstep(pentagram, pentagram - 0.05, dist) * 0.7);
        
        // Central glow
        float centralGlow = 1.0 - smoothstep(0.0, 0.15, dist);
        symbol = max(symbol, centralGlow);
        
        // Add magical swirls
        float swirl = sin(angle * 3.0 + u_time * 2.0 + dist * 10.0) * 0.5 + 0.5;
        symbol *= (0.7 + 0.3 * swirl);
        
        return symbol;
    }
    else { // Crystal particles - geometric crystal shape
        // Hexagonal crystal shape
        vec2 q = vec2(abs(center.x), center.y);
        float hex = max(q.x * 0.866 + q.y * 0.5, q.y);
        float crystal = 1.0 - smoothstep(0.3, 0.4, hex);
        
        // Add internal facets
        float facets = max(
            smoothstep(0.02, 0.0, abs(center.x - center.y * 0.577)),
            smoothstep(0.02, 0.0, abs(center.x + center.y * 0.577))
        );
        
        // Add prismatic effects
        float prism = sin(fs_in.worldPos.x * 0.1 + fs_in.worldPos.z * 0.15 + u_time) * 0.5 + 0.5;
        crystal += facets * 0.3 + prism * 0.2;
        
        return crystal;
    }
}

void main() {
    vec2 uv = fs_in.texCoord;
    
    // Generate procedural particle shape
    float shape = generateParticleShape(uv, fs_in.type);
    
    // Distance-based alpha scaling for performance
    float distanceAlpha = clamp(100.0 / fs_in.distanceToCamera, 0.1, 1.0);
    
    // Age-based alpha fade
    float ageFade = 1.0;
    if (fs_in.type < 0.5) { // Sparkles fade quickly at the end
        ageFade = 1.0 - pow(fs_in.ageFactor, 3.0);
    } else if (fs_in.type < 1.5) { // Embers fade gradually
        ageFade = 1.0 - fs_in.ageFactor;
    } else if (fs_in.type < 2.5) { // Magic particles have complex fade
        ageFade = sin(fs_in.ageFactor * 3.14159) * (1.0 - fs_in.ageFactor * 0.3);
    } else { // Crystal particles fade with sparkle
        ageFade = (1.0 - fs_in.ageFactor) * (0.7 + 0.3 * sin(u_time * 3.0));
    }
    
    // Combine color and shape
    vec4 finalColor = fs_in.color;
    finalColor.a *= shape * distanceAlpha * ageFade;
    
    // Add subtle HDR glow for magical particles
    if (fs_in.type > 1.5) {
        finalColor.rgb *= (1.0 + shape * 0.5); // HDR boost for bright areas
    }
    
    // Early discard for very transparent pixels
    if (finalColor.a < 0.02) {
        discard;
    }
    
    FragColor = finalColor;
}