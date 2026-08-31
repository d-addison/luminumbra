#version 450 core

// ===========================================================================
// GPU particle framework fragment stage.
//
// Forward-lit (sun + ambient + nearest point lights) emissive billboard with
// soft-particle alpha fade against the scene depth. Blended into the lit HDR
// (RGBA16F) lighting target by ParticlePass.
//
// The input interface block (VS_OUT) is the SAME block written by both
// magical_particles.vert (live vert->frag link) and magical_particles.geom
// (inventory vert->geom->frag link), so the fragment stage links in both.
// ===========================================================================

out vec4 FragColor;

in VS_OUT {
    vec2  texCoord;
    vec4  color;
    flat float atlasLayer;
    float distanceToCamera;
    float viewDepth;
    vec3  worldPos;
    flat float streakAspect;
} fs_in;

uniform float u_time;
uniform vec3  u_cameraPos;
uniform vec2  u_screenSize;
uniform float u_nearPlane;
uniform float u_farPlane;

// Forward lighting.
uniform vec3  u_sunDirection;  // light travel direction
uniform vec3  u_sunColor;
uniform float u_sunIntensity;
uniform vec3  u_ambientColor;

struct PointLight {
    vec3  position;
    vec3  color;
    float radius;
    float intensity;
};
uniform int        u_pointLightCount;
uniform PointLight u_pointLights[4];

// Scene depth for soft-particle fade.
uniform sampler2D u_sceneDepth;

// Linearize a hardware depth-buffer sample to view-space depth (positive).
float linearize_depth(float d) {
    float z = d * 2.0 - 1.0; // NDC
    return (2.0 * u_nearPlane * u_farPlane) /
           (u_farPlane + u_nearPlane - z * (u_farPlane - u_nearPlane));
}

// Radial soft sprite mask (round flakes / magical sparkles).
float sprite_mask(vec2 uv) {
    float dist = length(uv - vec2(0.5));
    return 1.0 - smoothstep(0.25, 0.5, dist);
}

// streak (rain) mask. The quad was elongated along
// its local Y in the vertex stage; uv.x is the ACROSS axis (thin) and uv.y the
// ALONG axis (the streak length). A near-vertical bright filament with a soft
// cross-falloff + rounded ends so rain reads as a light translucent STREAK, not a
// dot. A faint bright head (leading end) gives the streak a directional read.
float streak_mask(vec2 uv) {
    float across = abs(uv.x - 0.5) * 2.0;            // 0 at the spine, 1 at edges
    float along  = uv.y;                              // 0..1 down the streak
    // Thin bright core across the width; soft edge so it is translucent, not hard.
    float width = 1.0 - smoothstep(0.18, 0.62, across);
    // Fade the two ends so the streak has rounded, tapered tips (no hard caps).
    float ends = smoothstep(0.0, 0.12, along) * (1.0 - smoothstep(0.86, 1.0, along));
    return width * ends;
}

void main() {
    vec2 uv = fs_in.texCoord;
    bool isStreak = fs_in.streakAspect > 1.5;
    // rain-impact FOAM is tagged with a dedicated atlas
    // layer sentinel (precip_splash.json atlas_layer = 4) so it renders as a clean
    // whitish foam burst rather than a scene-tinted emissive sprite. All other
    // round (non-streak) particles (e.g. magical sparkles) keep the emissive path.
    bool isFoam = (!isStreak) && (fs_in.atlasLayer > 3.5);
    float shape = isStreak ? streak_mask(uv): sprite_mask(uv);
    if (shape <= 0.0) {
        discard;
    }

    // --- Soft-particle fade: shrink alpha where the billboard nears opaque
    // geometry, reading the G-buffer depth. ---
    vec2 screenUV = gl_FragCoord.xy / u_screenSize;
    float sceneDepthRaw = texture(u_sceneDepth, screenUV).r;
    float sceneViewDepth = linearize_depth(sceneDepthRaw);
    float softFade = clamp((sceneViewDepth - fs_in.viewDepth) / 1.5, 0.0, 1.0);

    // --- Forward lighting (the billboard faces the camera; use the toward-sun
    // half-Lambert plus ambient and nearby point lights). ---
    vec3 normal = normalize(u_cameraPos - fs_in.worldPos);
    float ndl = max(dot(normal, -normalize(u_sunDirection)), 0.0);
    vec3 lit = u_ambientColor + u_sunColor * u_sunIntensity * (0.5 + 0.5 * ndl);

    for (int i = 0; i < u_pointLightCount; ++i) {
        vec3 toLight = u_pointLights[i].position - fs_in.worldPos;
        float d = length(toLight);
        float atten = clamp(1.0 - d / max(u_pointLights[i].radius, 0.001), 0.0, 1.0);
        atten *= atten;
        lit += u_pointLights[i].color * u_pointLights[i].intensity * atten;
    }

    vec4 finalColor = fs_in.color;
    if (isStreak) {
        // CLEAN RAIN. The owner saw "coloured TV static"
        // -- cyan/teal/pink speckle. Root cause: the streak colour was modulated by
        // the scene forward-lighting term (`lit` = ambient + sun + up to 4 coloured
        // point lights), so every streak picked up a per-position hue and the field
        // read as chromatic noise. The fix: rain is a CONSTANT light blue-white
        // translucent filament with NO per-particle hue variation and NO scene-light
        // tint. A single fixed water colour; the only spatial variation is the
        // soft cross-section mask (spine brighter than edges) and the depth fade --
        // i.e. shape, not colour. This reads as clean rain, not static.
        const vec3 kRainColor = vec3(0.82, 0.90, 1.0);   // fixed light blue-white
        finalColor.rgb = kRainColor;
        // Brighten the thin spine a touch so the centre of each streak catches a
        // wet highlight running down the filament (still hue-neutral: white add).
        finalColor.rgb += vec3(0.10) * shape;
        // Translucent veil: moderate peak alpha, shaped by the streak mask so it
        // is soft-edged. Kept well below 1 so the rain stays see-through in motion.
        finalColor.a = clamp(fs_in.color.a, 0.0, 1.0) * shape * 0.85 * softFade;
    } else if (isFoam) {
        // SPLASH = subtle whitish foam burst, not coloured
        // dots. The impact spray previously ran through the emissive path
        // (scene-light tint + additive HDR glow core), which painted small COLOURED
        // speckle scattered around the ground line. Now the splash is a fixed pale
        // near-white foam with a soft round mask and a low alpha, with NO scene-light
        // tint and NO additive glow -- a faint white burst on impact, nothing more.
        const vec3 kFoamColor = vec3(0.92, 0.95, 1.0);   // pale near-white foam
        finalColor.rgb = kFoamColor;
        finalColor.a = clamp(fs_in.color.a, 0.0, 1.0) * shape * 0.5 * softFade;
    } else {
        // Emissive core: the particle is its own light source, modulated by the
        // forward-lit term so it still reads the scene's mood. (Magical sparkles.)
        finalColor.rgb *= (0.6 + 0.4 * lit);
        finalColor.rgb += fs_in.color.rgb * shape * 0.5; // HDR glow core
        finalColor.a *= shape * softFade;
    }

    if (finalColor.a < 0.01) {
        discard;
    }

    FragColor = finalColor;
}
