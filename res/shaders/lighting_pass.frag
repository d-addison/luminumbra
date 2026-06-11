#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

// G-Buffer samplers (match g_buffer.frag output names)
uniform sampler2D gPosition;          // RGB16F: full view-space position
uniform sampler2D gNormalMaterial;    // RGB10A2: Octahedral normal + material ID  
uniform sampler2D gAlbedoRoughness;   // RGBA8: RGB albedo + roughness
uniform sampler2D gMetallicAO;        // RG16F: Metallic + AO
uniform sampler2D u_ssao;

// Material lookup
uniform sampler2D u_materialLUT;

// G-Buffer decoding functions
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}

vec3 decode_octahedral(vec2 encoded) {
    vec2 e = encoded * 2.0 - 1.0;
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0) v.xy = octWrap(v.xy);
    return normalize(v);
}

// Shadow and other uniforms
uniform sampler2DArray u_shadowCascades;
uniform mat4 u_lightSpaceMatrices[4];
uniform vec4 u_cascadeSplits; 

// Texture and world uniforms
uniform sampler2DArray u_terrainTextures;
uniform sampler2D u_causticsTexture;
uniform float u_time;
uniform float u_sea_level;

uniform vec3 u_terrainOrigin;
uniform vec3 u_viewPos;
uniform mat4 u_inverseView;
uniform vec3 u_skyAmbientColor;
struct SunLight {
    vec3 direction;
    vec3 color;
};
uniform SunLight u_sun;

#define MAX_POINT_LIGHTS 32
struct PointLight {
    vec3 position;
    float radius;
    vec3 color;
    float intensity;
};
uniform PointLight u_pointLights[MAX_POINT_LIGHTS];
uniform int u_pointLightCount;

const float PI = 3.14159265359;

// Optimized PBR functions with precalculated values
float DistributionGGX(float NdotH, float a2) {
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float k) {
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
float CalculateShadow(vec3 fragPos, vec3 normal, vec3 lightDir, float viewDepth) {
    // 1. Determine which cascade to use in a branchless way
    int cascadeIndex = 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.x) ? 1 : 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.y) ? 1 : 0;
    cascadeIndex += (viewDepth > u_cascadeSplits.z) ? 1 : 0;

    // 2. Project fragment into the chosen light space
    vec4 fragPosLightSpace = u_lightSpaceMatrices[cascadeIndex] * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if(projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0) {
        // Outside the last cascade's light frustum: no shadow information
        // exists, so treat the fragment as LIT (standard CSM out-of-range
        // convention). Returning 0.0 here rendered everything beyond the
        // shadow range sun-unlit - invisible before the far-LOD horizon
        // existed (T-I3-9), pitch-black mountains after it.
        return 1.0;
    }

    // 3. PCF (Percentage-Closer Filtering)
    float currentDepth = projCoords.z;
    float shadow = 0.0;
    float bias = max(0.005 * (1.0 - dot(normal, lightDir)), 0.001);
    vec2 texelSize = 1.0 / vec2(textureSize(u_shadowCascades, 0).xy);
    
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(u_shadowCascades, vec3(projCoords.xy + vec2(x, y) * texelSize, cascadeIndex)).r; 
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;        
        }
    }
    return 1.0 - (shadow / 9.0);
}
vec3 TriPlanar(vec3 worldPos, vec3 normal, sampler2DArray texArray, float layer, float scale) {
    vec3 relPos = worldPos - u_terrainOrigin; vec2 uv_y=relPos.xz*scale; vec2 uv_x=relPos.zy*scale; vec2 uv_z=relPos.xy*scale;
    vec3 tex_y=texture(texArray,vec3(uv_y,layer)).rgb; vec3 tex_x=texture(texArray,vec3(uv_x,layer)).rgb; vec3 tex_z=texture(texArray,vec3(uv_z,layer)).rgb;
    vec3 weights=pow(abs(normal),vec3(3.0)); weights=normalize(weights);
    return tex_y*weights.y+tex_x*weights.x+tex_z*weights.z;
}

vec3 CalculateLightContribution(vec3 L, vec3 V, vec3 N, vec3 F0, vec3 albedo, float metallic, float a2, float k, vec3 radiance) {
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Optimized PBR calculations with precalculated values
    float NDF = DistributionGGX(NdotH, a2);
    float G   = GeometrySmith(NdotV, NdotL, k);
    vec3  F   = fresnelSchlick(HdotV, F0);
    
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    
    // Optimized specular calculation
    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 0.0001);
      
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

void main() {
    // --- Step 1: Decode compressed G-Buffer ---
    
    vec3 viewPos = texture(gPosition, TexCoords).rgb;
    
    // Decode octahedral normal and material ID
    vec4 normalData = texture(gNormalMaterial, TexCoords);
    vec3 viewNormal = decode_octahedral(normalData.xy);
    uint MaterialID = uint(round(normalData.a * 255.0));
    
    // Get albedo and roughness
    vec4 albedoData = texture(gAlbedoRoughness, TexCoords);
    vec3 Albedo = albedoData.rgb;
    float Roughness = clamp(albedoData.a, 0.05, 1.0);
    
    // Get metallic and AO
    vec2 matData = texture(gMetallicAO, TexCoords).xy;
    float Metallic = matData.r;
    float ao = texture(u_ssao, TexCoords).r;
    
    // Transform to world space
    vec3 FragPos = vec3(u_inverseView * vec4(viewPos, 1.0));
    vec3 Normal = normalize(mat3(u_inverseView) * viewNormal);

    // Use tri-planar textures only for material IDs backed by terrain texture layers.
    // Crystal, water, or invalid IDs keep their G-buffer albedo instead of sampling
    // outside the texture array and producing undefined material patches.
    int terrainLayerCount = textureSize(u_terrainTextures, 0).z;
    if (MaterialID > 0u && int(MaterialID) <= terrainLayerCount) {
        Albedo = TriPlanar(FragPos, Normal, u_terrainTextures, float(MaterialID - 1u), 0.1);
    }

    vec3 V = normalize(u_viewPos - FragPos);
    vec3 F0 = mix(vec3(0.04), Albedo, Metallic);
    
    // Precalculate roughness-dependent values once
    float a = Roughness * Roughness;
    float a2 = a * a;
    float r = Roughness + 1.0;
    float k = (r * r) / 8.0;

    // --- LIGHTING CALCULATION ---
    vec3 Lo = vec3(0.0);
    vec3 L_sun = normalize(u_sun.direction);
    float shadow = CalculateShadow(FragPos, Normal, L_sun, abs(viewPos.z));
    Lo += CalculateLightContribution(L_sun, V, Normal, F0, Albedo, Metallic, a2, k, u_sun.color) * shadow;

    // Point Lights with early rejection
    for (int i = 0; i < u_pointLightCount; ++i) {
        vec3 lightVec = u_pointLights[i].position - FragPos;
        float dist = length(lightVec);
        
        // Early rejection for lights out of range
        if (dist < u_pointLights[i].radius) {
            float attenuation = 1.0 - smoothstep(0.0, u_pointLights[i].radius, dist);
            vec3 L_point = lightVec / dist; // Avoid normalize when we already have distance
            vec3 radiance = u_pointLights[i].color * u_pointLights[i].intensity * attenuation;
            Lo += CalculateLightContribution(L_point, V, Normal, F0, Albedo, Metallic, a2, k, radiance);
        }
    }

    // --- NEW: CAUSTICS CALCULATION ---
    vec3 caustics = vec3(0.0);
    // Apply caustics only if fragment is below sea level and on an upward-facing surface
    if (FragPos.y < u_sea_level && Normal.y > 0.5) {
        vec2 uv1 = FragPos.xz * 0.2 + vec2(u_time * 0.01, u_time * 0.015);
        vec2 uv2 = FragPos.xz * 0.15 - vec2(u_time * 0.02, u_time * 0.01);
        
        float caustic1 = texture(u_causticsTexture, uv1).r;
        float caustic2 = texture(u_causticsTexture, uv2).r;
        
        // Sum and modulate by sun intensity and shadow
        caustics = (caustic1 + caustic2) * u_sun.color * shadow * 0.5;
    }

    // --- MAGICAL CRYSTAL EFFECTS ---
    vec3 crystalGlow = vec3(0.0);
    // Fix: MaterialID is already decoded from normalData.a above, not matData.a
    
    if (MaterialID == 6u) { // Luminous Crystal
        // Inner magical glow
        float glowPulse = sin(u_time * 2.0) * 0.3 + 0.7;
        vec3 magicColor = vec3(0.6, 0.8, 1.0) * glowPulse * 0.8;
        
        // Fresnel-based edge lighting
        float fresnel = pow(1.0 - max(0.0, dot(V, Normal)), 3.0);
        vec3 edgeGlow = vec3(0.4, 0.7, 1.0) * fresnel * 1.5;
        
        // Prismatic dispersion effect
        float dispersion = sin(u_time * 1.5 + FragPos.x * 0.1 + FragPos.z * 0.15);
        vec3 prismColors = vec3(
            0.8 + 0.4 * sin(dispersion),
            0.6 + 0.4 * sin(dispersion + 2.0),
            1.0 + 0.2 * sin(dispersion + 4.0)
        );
        
        // Energy field around crystals
        float energyField = abs(sin(u_time * 3.0 + length(FragPos) * 0.05)) * 0.3;
        
        crystalGlow = magicColor + edgeGlow + prismColors * energyField * 0.2;
        crystalGlow *= 1.5; // HDR boost for magical effect
    }
    
    // --- Final Color Composition ---
    vec3 ambient = u_skyAmbientColor * Albedo * ao;
    vec3 color = ambient + Lo + caustics + crystalGlow; // Add magical crystal glow

    // Enhanced HDR tone mapping for magical effects
    // Use filmic tone mapping to preserve magical highlights
    color = color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14);
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));

    FragColor = vec4(color, 1.0);
}
