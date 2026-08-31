#version 450 core

out vec4 FragColor;

in vec2 TexCoords;

// Scene textures
uniform sampler2D u_sceneColor;
uniform sampler2D u_sceneDepth;
uniform sampler2D gPosition;
uniform sampler2D gNormal;

// Weather parameters
uniform float u_time;
uniform vec3 u_cameraPos;
uniform mat4 u_inverseView;
uniform mat4 u_inverseProjection;

// Weather state
uniform float u_rainIntensity = 0.0;      // 0.0 = no rain, 1.0 = heavy rain
uniform float u_snowIntensity = 0.0;      // 0.0 = no snow, 1.0 = heavy snow
uniform float u_fogDensity = 0.0;         // 0.0 = clear, 1.0 = thick fog
uniform float u_stormIntensity = 0.0;     // 0.0 = calm, 1.0 = storm
uniform vec3 u_windDirection = vec3(1.0, 0.0, 0.0);
uniform float u_windStrength = 0.5;
// local precipitation -> material WETNESS response.:
// darkens albedo and adds a sun-glossy sheen on wet (upward-facing) surfaces.
// Fed from the replicated WeatherSystem precipitation; never written to the sim.
uniform float u_wetness = 0.0;

// Lighting
uniform vec3 u_sunDirection;
uniform vec3 u_sunColor;
uniform float u_sunIntensity;

// Enhanced noise functions
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
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

float noise3D(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(
        mix(mix(hash3(i + vec3(0,0,0)), hash3(i + vec3(1,0,0)), f.x),
            mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), f.x),
            mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), f.x), f.y), f.z);
}

// Fractal noise for weather patterns
float fbm(vec3 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for(int i = 0; i < octaves; i++) {
        value += amplitude * noise3D(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}

// Reconstruct world position from depth
vec3 worldPosFromDepth(vec2 uv, float depth) {
    float z = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 viewPos = u_inverseProjection * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos = u_inverseView * viewPos;
    return worldPos.xyz;
}

// Thin per-column vertical streak field. hash01(column) selects which columns
// carry a streak; a fract phase scrolls them downward over time. This is a
// 1D-along-X structure (NOT a 2D value-noise blob field), so it reads as faint
// vertical RAIN STREAKS rather than the old "lens-dirt" speckle.
float rainStreaks(vec2 uv, float density, float speed, float colScale) {
    float col = floor(uv.x * colScale);
    float pick = hash(vec2(col, 3.17));          // which columns carry a streak
    if (pick > density) return 0.0;
    // Per-column random length + phase so streaks are staggered, not a grid.
    float seed = hash(vec2(col, 9.71));
    float yphase = fract(uv.y * (5.0 + seed * 5.0) + u_time * speed + seed);
    // A short bright dash within the column with soft ends (tapered streak).
    float dash = smoothstep(0.0, 0.06, yphase) * (1.0 - smoothstep(0.18, 0.5, yphase));
    // Thin across the column width (centred), soft-edged.
    float across = abs(fract(uv.x * colScale) - 0.5) * 2.0;
    float width = 1.0 - smoothstep(0.25, 0.8, across);
    return dash * width;
}

// Rain effect.
//
// the old screen-space rain painted a 2D value
// noise (noise(screenUV*vec2(80,200))) over EVERY pixel -- blobby grain across
// the whole sky dome, the "lens-dirt" speckle the owner saw. The bulk rain is now
// the 3D ParticlePass streak volume around the camera; this screen-space term is
// reduced to a FAINT, properly VERTICAL streak veil (thin per-column dashes, not
// 2D blobs) that adds gentle depth/atmosphere to the rain without the speckle. It
// is kept low-contrast so it never reads as dirt, and a second sparser/slower
// layer adds parallax. We also keep the ground wet-sheen response.
vec3 renderRain(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_rainIntensity < 0.01) return sceneColor;

    vec3 rainColor = sceneColor;

    // Two faint vertical streak layers (near + far) for a subtle rain veil. Wind
    // sway shifts the column sample so the veil leans with the wind.
    vec2 swayUV = screenUV;
    swayUV.x += u_windDirection.x * u_windStrength * (screenUV.y) * 0.08;
    float near = rainStreaks(swayUV, 0.42, 1.9, 150.0);
    float far  = rainStreaks(swayUV * vec2(1.0, 1.3), 0.30, 1.2, 96.0);
    float veil = (near * 0.7 + far * 0.45) * u_rainIntensity;
    // Light water-white, low intensity -> a translucent veil, not bright dirt.
    vec3 veilColor = vec3(0.82, 0.9, 1.0) * (0.35 + u_stormIntensity * 0.25);

    // KILL THE DOWN-VIEW GREY VEIL. This screen-space veil
    // paints fixed VERTICAL screen-space dashes over EVERY pixel. When the camera
    // pitches DOWN, the frame fills with NEAR terrain, and those vertical dashes
    // smear into a flat desaturated grey haze across the ground -- the "grey fog,
    // not streaks" defect. Vertical screen streaks only read as falling rain when
    // they overlay DISTANT scene / sky (a roughly horizontal look). So we fade the
    // veil out as the viewed surface gets close to the camera (i.e. looking down at
    // the ground). The 3D ParticlePass streaks carry the actual rain in the down
    // view; this veil is reserved for the far/horizon read where it belongs.
    float viewDist = length(worldPos - u_cameraPos);
    // Full veil only past ~40m (horizon/sky); fully suppressed within ~12m (the
    // near terrain that floods a down-pitched frame).
    float farVeilMask = smoothstep(12.0, 40.0, viewDist);
    rainColor += veilColor * veil * 0.20 * farVeilMask;

    // Ground sheen ONLY on valid upward-facing surfaces (sky has no normal, so it
    // is untouched). A slow fbm gives a gentle living wet-ground shimmer.
    vec3 worldNormal = texture(gNormal, screenUV).rgb;
    if (length(worldNormal) > 0.1) {
        float splashFactor = max(0.0, dot(normalize(worldNormal), vec3(0, 1, 0)));
        float splash = fbm(worldPos * 2.0 + vec3(u_time * 0.5), 2) * splashFactor;
        splash = smoothstep(0.45, 0.85, splash) * u_rainIntensity;
        rainColor += vec3(0.35, 0.42, 0.52) * splash * 0.12;
    }

    return rainColor;
}

// Snow effect
vec3 renderSnow(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_snowIntensity < 0.01) return sceneColor;

    vec3 snowColor = sceneColor;

    // Multiple snow layers for depth
    for(int layer = 0; layer < 3; layer++) {
        float layerScale = 1.0 + float(layer) * 0.5;
        float layerSpeed = 1.0 - float(layer) * 0.2;

        vec2 snowUV = screenUV * vec2(60.0, 60.0) * layerScale;
        snowUV.y += u_time * 3.0 * layerSpeed; // Gentle falling
        snowUV.x += sin(u_time * 0.5 + snowUV.y * 0.2) * 2.0; // Wind drift

        float snowPattern = noise(snowUV);
        snowPattern = smoothstep(0.85, 0.95, snowPattern);

        // Distance falloff. Clamped like rain: near-field flakes stay
        // visible against far-plane sky pixels.
        float distance = length(worldPos - u_cameraPos);
        float snowFalloff = exp(-min(distance, 100.0) * 0.008);

        // Layer depth effect
        float layerIntensity = u_snowIntensity * (1.0 - float(layer) * 0.3);

        vec3 snowFlakeColor = vec3(0.95, 0.98, 1.0) * (0.8 + float(layer) * 0.1);
        float snowStrength = snowPattern * layerIntensity * snowFalloff;

        snowColor = mix(snowColor, snowFlakeColor, snowStrength * 0.4);
    }

    // Snow accumulation effect on surfaces
    vec3 worldNormal = texture(gNormal, screenUV).rgb;
    if (length(worldNormal) > 0.1) {
        float accumulation = max(0.0, dot(worldNormal, vec3(0, 1, 0))); // Upward facing
        accumulation *= u_snowIntensity * 0.3;
        snowColor = mix(snowColor, vec3(0.9, 0.95, 1.0), accumulation);
    }

    return snowColor;
}

// Fog effect with enhanced scattering
vec3 renderFog(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_fogDensity < 0.01) return sceneColor;

    float distance = length(worldPos - u_cameraPos);
    float height = worldPos.y;

    // Height-based fog density (more fog at lower elevations)
    float heightFactor = exp(-max(0.0, height - (-10.0)) * 0.1);

    // Volumetric fog with noise
    vec3 fogSamplePos = worldPos * 0.01 + vec3(u_time * 0.02, 0.0, u_time * 0.015);
    float fogNoise = fbm(fogSamplePos, 4);
    fogNoise = fogNoise * 0.5 + 0.5; // Normalize

    // Distance-based fog accumulation
    float fogFactor = 1.0 - exp(-distance * u_fogDensity * 0.01 * heightFactor * fogNoise);

    // Fog color based on time of day and sun direction
    vec3 fogColor = mix(
        vec3(0.7, 0.8, 0.9),  // Day fog
        vec3(0.3, 0.4, 0.6),  // Night fog
        1.0 - u_sunIntensity
    );

    // Sun scattering in fog
    vec3 viewDir = normalize(worldPos - u_cameraPos);
    float sunDot = dot(viewDir, -u_sunDirection);
    float sunScatter = pow(max(0.0, sunDot), 8.0) * u_sunIntensity;
    fogColor += u_sunColor * sunScatter * 0.3;

    return mix(sceneColor, fogColor, fogFactor);
}

// Lightning effect for storms
vec3 renderLightning(vec3 sceneColor, vec2 screenUV) {
    if (u_stormIntensity < 0.3) return sceneColor;

    // Random lightning flashes
    float lightningTime = u_time * 0.1;
    float lightningChance = hash(vec2(floor(lightningTime), floor(lightningTime * 0.7)));

    if (lightningChance > 0.98) { // Rare lightning flashes
        float flashIntensity = hash(vec2(floor(lightningTime * 10.0))) * u_stormIntensity;

        // Lightning branch pattern
        float branch = fbm(vec3(screenUV * 50.0, u_time * 10.0), 3);
        branch = smoothstep(0.6, 0.8, branch);

        vec3 lightningColor = vec3(0.8, 0.9, 1.0) * flashIntensity * 2.0;
        sceneColor += lightningColor * (0.1 + branch * 0.3);
    }

    return sceneColor;
}

// Wind distortion effect
vec3 renderWind(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_windStrength < 0.1) return sceneColor;

    // Wind-based screen distortion for vegetation and particles
    vec2 windOffset = vec2(
        sin(u_time * 2.0 + worldPos.x * 0.1) * u_windStrength * 0.002,
        cos(u_time * 1.5 + worldPos.z * 0.1) * u_windStrength * 0.001
    );

    // Sample with slight offset for wind effect
    vec2 distortedUV = screenUV + windOffset;
    distortedUV = clamp(distortedUV, vec2(0.0), vec2(1.0));

    return texture(u_sceneColor, distortedUV).rgb;
}

// wetness material response. On upward-facing solid
// surfaces, local precipitation darkens the albedo (wet ground reads darker) and
// adds a view-dependent specular sheen toward the sun (wet surfaces glisten). The
// sim provides u_wetness (local precip intensity); this writes only the rendered
// color and never feeds back into sim/world_hash.
vec3 renderWetness(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_wetness < 0.01) return sceneColor;
    vec3 worldNormal = texture(gNormal, screenUV).rgb;
    if (length(worldNormal) < 0.1) return sceneColor; // sky / no surface
    worldNormal = normalize(worldNormal);
    float upFacing = max(0.0, dot(worldNormal, vec3(0.0, 1.0, 0.0)));
    float wet = u_wetness * upFacing;
    if (wet < 0.01) return sceneColor;

    // Darken albedo on wet surfaces (water film absorbs).
    vec3 wetColor = sceneColor * (1.0 - 0.35 * wet);

    // Specular sheen: reflect the view direction about the surface normal and
    // measure alignment with the sun for a glossy highlight.
    vec3 viewDir = normalize(worldPos - u_cameraPos);
    vec3 reflectDir = reflect(viewDir, worldNormal);
    float spec = pow(max(0.0, dot(reflectDir, -u_sunDirection)), 32.0);
    wetColor += u_sunColor * spec * u_sunIntensity * wet * 0.5;
    return mix(sceneColor, wetColor, wet);
}

void main() {
    vec2 screenUV = TexCoords;

    // Get scene data
    vec3 sceneColor = texture(u_sceneColor, screenUV).rgb;
    float sceneDepth = texture(u_sceneDepth, screenUV).r;
    vec3 worldPos = worldPosFromDepth(screenUV, sceneDepth);

    // Start with wind distortion as base
    vec3 finalColor = renderWind(sceneColor, screenUV, worldPos);

    // wetness material response BEFORE the volumetric weather so the wet
    // surface tint is then occluded by fog/rain like the rest of the scene.
    finalColor = renderWetness(finalColor, screenUV, worldPos);

    // Apply weather effects in order
    finalColor = renderFog(finalColor, screenUV, worldPos);
    finalColor = renderRain(finalColor, screenUV, worldPos);
    finalColor = renderSnow(finalColor, screenUV, worldPos);
    // the legacy random fbm "lightning" here
    // (renderLightning) fired on its OWN screen-space hash schedule and painted a
    // smeared fbm splotch -- it competes with the deterministic lightning_overlay
    // bolt/flash and added more sky noise. The dedicated lightning_overlay pass
    // owns the strike now, so this redundant flash is removed.

    // Global weather tinting.
    // the storm must DIM the whole scene
    // (overcast dome + darkened terrain) so the rain streaks read as bright water
    // over a dark backdrop and the lightning bolt + flash have contrast. The old
    // factor (0.7 + storm*0.2) barely darkened (>=0.9) and left a bright clear-blue
    // sky behind the strike. Darken substantially with storm intensity and pull a
    // cool, desaturated overcast cast across the frame.
    if (u_stormIntensity > 0.05) {
        float dim = mix(1.0, 0.36, clamp(u_stormIntensity, 0.0, 1.0));
        finalColor *= dim;
        // Desaturate + cool the overcast (grey-blue storm light).
        float luma = dot(finalColor, vec3(0.299, 0.587, 0.114));
        vec3 overcast = mix(vec3(luma), finalColor, 0.55) * vec3(0.86, 0.92, 1.04);
        finalColor = mix(finalColor, overcast, clamp(u_stormIntensity, 0.0, 1.0) * 0.8);

        //  (defect 3): NIGHT-STORM legibility floor on
        // SURFACES. At night the lit scene is near-black and the storm dim above
        // crushes it the rest of the way, so a night storm read as an empty black
        // frame. Lift a faint cool storm-ambient floor on solid surfaces (the sky
        // has no normal, so it is untouched and stays a dark dome). Only meaningful
        // where the surface is already very dark (night), so the daytime storm is
        // unaffected. The max never darkens -- it only sets a minimum.
        vec3 stormNormal = texture(gNormal, screenUV).rgb;
        if (length(stormNormal) > 0.1) {
            float surfLuma = dot(finalColor, vec3(0.299, 0.587, 0.114));
            float floorLift = clamp(u_stormIntensity, 0.0, 1.0)
                              * (1.0 - smoothstep(0.0, 0.10, surfLuma)) * 0.045;
            finalColor += vec3(0.7, 0.8, 1.0) * floorLift;
        }
    }

    if (u_fogDensity > 0.1) {
        // Fog desaturation
        float luminance = dot(finalColor, vec3(0.299, 0.587, 0.114));
        finalColor = mix(finalColor, vec3(luminance), u_fogDensity * 0.4);
    }

    FragColor = vec4(finalColor, 1.0);
}