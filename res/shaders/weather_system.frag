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

// Fog effect with enhanced scattering
vec3 renderFog(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_fogDensity < 0.01) return sceneColor;

    float distance = length(worldPos - u_cameraPos);
    // Integrate the 10 m ground layer along the entire ray. Using only the
    // destination's height filled the clear air above it with lowland fog,
    // making a downward view from 1200 m look covered in white water.
    float h0 = u_cameraPos.y + 10.0;
    float h1 = worldPos.y + 10.0;
    float low = min(h0, h1);
    float high = max(h0, h1);
    float heightFactor;
    if (high <= 0.0) {
        heightFactor = 1.0;
    } else if (high - low < 0.001) {
        heightFactor = exp(-max(0.0, 0.5 * (h0 + h1)) * 0.1);
    } else {
        float denseLength = max(0.0, -low);
        float airIntegral = 10.0 * (exp(-max(0.0, low) * 0.1) - exp(-high * 0.1));
        heightFactor = (denseLength + airIntegral) / (high - low);
    }

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

// wetness material response. On upward-facing solid
// surfaces, local precipitation darkens the albedo (wet ground reads darker) and
// adds a view-dependent specular sheen toward the sun (wet surfaces glisten). The
// sim provides u_wetness (local precip intensity); this writes only the rendered
// color and never feeds back into sim/world_hash.
vec3 renderWetness(vec3 sceneColor, vec2 screenUV, vec3 worldPos) {
    if (u_wetness < 0.01) return sceneColor;
    if (texture(u_sceneDepth, screenUV).r >= 1.0) return sceneColor;
    vec2 encoded = texture(gNormal, screenUV).rg * 2.0 - 1.0;
    vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    vec3 worldNormal = normalize(mat3(u_inverseView) * normalize(n));
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

    // Wind bends world-space vegetation and particles; opaque terrain stays fixed.
    vec3 finalColor = sceneColor;

    // wetness material response BEFORE the volumetric weather so the wet
    // surface tint is then occluded by fog/rain like the rest of the scene.
    finalColor = renderWetness(finalColor, screenUV, worldPos);

    // Apply weather effects in order
    finalColor = renderFog(finalColor, screenUV, worldPos);
    // Rain and snow are depth-tested world-space particles. Snow ground cover
    // is accumulated separately by SnowCoverModel and shaded in the lighting pass.

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
        if (sceneDepth < 1.0) {
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
