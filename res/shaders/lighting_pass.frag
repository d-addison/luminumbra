#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

// G-Buffer samplers (containing view-space data)
uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedo;
uniform sampler2D gMaterial; 
uniform sampler2D u_ssao;

// Matrix to transform from view space back to world space
uniform mat4 u_inverseView;

// Shadow and other uniforms
uniform sampler2DArray u_shadowCascades;
uniform mat4 u_lightSpaceMatrices[4];
// u_cascadeSplits now contains the linear view-space distances for each cascade end point
uniform vec4 u_cascadeSplits; 

// Texture and world uniforms
uniform sampler2DArray u_terrainTextures;
uniform vec3 u_terrainOrigin;
uniform vec3 u_viewPos;
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

// ----------------------------------------------------------------------------
// Tri-Planar mapping using terrain-relative coordinates.
// This projects textures from three axes (X, Y, Z) and blends them based on
// the surface normal to avoid texture stretching on steep slopes.
// Using 'relPos' (relative position) prevents texture swimming.
// ----------------------------------------------------------------------------
vec3 TriPlanar(vec3 worldPos, vec3 normal, sampler2DArray texArray, float layer, float scale) {
    vec3 relPos = worldPos - u_terrainOrigin;

    // UV coordinates are the same
    vec2 uv_y = relPos.xz * scale;
    vec2 uv_x = relPos.zy * scale;
    vec2 uv_z = relPos.xy * scale;

    // Sample the texture array using 3D coordinates (u, v, layer)
    vec3 tex_y = texture(texArray, vec3(uv_y, layer)).rgb;
    vec3 tex_x = texture(texArray, vec3(uv_x, layer)).rgb;
    vec3 tex_z = texture(texArray, vec3(uv_z, layer)).rgb;

    // Blending weights are the same
    vec3 weights = pow(abs(normal), vec3(3.0));
    weights = normalize(weights);

    return tex_y * weights.y + tex_x * weights.x + tex_z * weights.z;
}

vec3 CalculateLightContribution(vec3 L, vec3 V, vec3 N, vec3 F0, vec3 albedo, float metallic, float roughness, vec3 radiance)
{
    vec3 H = normalize(V + L);

    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    
    float NdotL = max(dot(N, L), 0.0);
    vec3 specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 0.001);
    
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

// --- PBR HELPER FUNCTIONS (UNCHANGED) ---
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return num / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// --- SHADOW CALCULATION (UNCHANGED) ---
float CalculateShadow(vec3 fragPos, vec3 normal, vec3 lightDir, float viewDepth) {
    vec4 fragPosLightSpace[4];
    for(int i = 0; i < 4; ++i) {
        fragPosLightSpace[i] = u_lightSpaceMatrices[i] * vec4(fragPos, 1.0);
    }

    int cascadeIndex = 0;
    if(viewDepth > u_cascadeSplits.x) cascadeIndex = 1;
    if(viewDepth > u_cascadeSplits.y) cascadeIndex = 2;
    if(viewDepth > u_cascadeSplits.z) cascadeIndex = 3;

    vec4 projCoords = fragPosLightSpace[cascadeIndex];
    projCoords.xyz /= projCoords.w;
    projCoords.xyz = projCoords.xyz * 0.5 + 0.5;

    float currentDepth = projCoords.z;
    if(currentDepth > 1.0) return 0.0;

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


void main() {
    // --- Step 1 & 2: Retrieve from G-buffer and reconstruct world space (unchanged) ---
    vec3 viewPos = texture(gPosition, TexCoords).rgb;
    vec3 viewNormal = normalize(texture(gNormal, TexCoords).rgb);
    vec3 FragPos = vec3(u_inverseView * vec4(viewPos, 1.0));
    vec3 Normal = normalize(mat3(u_inverseView) * viewNormal);

    // --- Step 3: Material and Lighting Calculation ---
    vec4 matData = texture(gMaterial, TexCoords);
    float Metallic = matData.r;
    float Roughness = clamp(matData.g, 0.05, 1.0);
    float ao = texture(u_ssao, TexCoords).r;

    uint MaterialID = uint(round(matData.a * 255.0));
    float textureScale = 0.1;

    vec3 Albedo = texture(gAlbedo, TexCoords).rgb; // Default to the G-Buffer albedo

    // OPTIMIZATION: Simplified material selection using the texture array
    if (MaterialID >= 1u && MaterialID <= 5u) {
        // MaterialIDs 1-5 map to array layers 0-4
        float layer_index = float(MaterialID - 1u); 
        float matTextureScale = textureScale;

        // Optional: Apply material-specific scaling
        if (MaterialID == 1u) matTextureScale *= 0.5;  // Stone
        if (MaterialID == 5u) matTextureScale *= 0.75; // Deepslate

        Albedo = TriPlanar(FragPos, Normal, u_terrainTextures, layer_index, matTextureScale);
    }

    // The rest of the PBR lighting calculation remains exactly the same
    vec3 V = normalize(u_viewPos - FragPos);
    vec3 F0 = mix(vec3(0.04), Albedo, Metallic);

    // ===================== LIGHTING CALCULATION REWORK =====================
    vec3 Lo = vec3(0.0); // Final outgoing radiance, start at black

    // 1. Directional Sun Light
    vec3 L_sun = normalize(u_sun.direction);
    float shadow = CalculateShadow(FragPos, Normal, L_sun, abs(viewPos.z));
    Lo += CalculateLightContribution(L_sun, V, Normal, F0, Albedo, Metallic, Roughness, u_sun.color) * shadow;

    // 2. Point Lights
    for (int i = 0; i < u_pointLightCount; ++i) {
        vec3 lightVec = u_pointLights[i].position - FragPos;
        float dist = length(lightVec);

        // Use radius to attenuate light
        float attenuation = 1.0 - smoothstep(0.0, u_pointLights[i].radius, dist);
        if (attenuation > 0.0) {
            vec3 L_point = normalize(lightVec);
            vec3 radiance = u_pointLights[i].color * u_pointLights[i].intensity * attenuation;
            Lo += CalculateLightContribution(L_point, V, Normal, F0, Albedo, Metallic, Roughness, radiance);
        }
    }

    // --- Final Color Composition ---
    vec3 ambient = u_skyAmbientColor * Albedo * ao;
    vec3 color = ambient + Lo;

    // Basic Reinhard tone mapping and Gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0/2.2));

    FragColor = vec4(color, 1.0);
}