#version 410
out vec4 FragColor;

in vec3 v_FragPos;
in vec3 v_Normal;
in float v_ClipDistance;
in vec4 v_FragPosLightSpace;

// Main Uniforms
uniform vec3 viewPos;
uniform vec3 fogColor;
uniform vec3 sunDirection;
uniform sampler2D shadowMap;
uniform float u_Wetness; // Range 0.0 (dry) to 1.0 (soaking wet)
uniform vec3 u_sandColor;
uniform vec3 u_grassColor;
uniform vec3 u_rockColor;
uniform vec3 u_snowColor;
uniform float u_sandLevel;
uniform float u_grassLevel;
uniform float u_rockLevel;
uniform float u_snowLevel;
uniform float u_blendRange; // How sharp/soft the transition between levels is

// Simple noise function to add variation
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float ShadowCalculation(vec4 fragPosLightSpace, vec3 normal)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0) {
        return 0.0;
    }

    float shadow = 0.0;
    // Use a dynamic bias based on the slope to prevent shadow acne
    float bias = max(0.05 * (1.0 - dot(normal, -sunDirection)), 0.005);
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);

    // Poisson Disk sampling offsets for smoother soft shadows
    vec2 poissonDisk[4] = vec2[](
      vec2( -0.94201624, -0.39906216 ),
      vec2( 0.94558609, -0.76890725 ),
      vec2( -0.094184101, -0.92938870 ),
      vec2( 0.34495938, 0.29387760 )
    );

    // 4-tap PCF
    for (int i = 0; i < 4; i++) {
        float pcfDepth = texture(shadowMap, projCoords.xy + poissonDisk[i] * texelSize * 1.5).r;
        if (projCoords.z - bias > pcfDepth) {
            shadow += 0.25;
        }
    }
    
    return shadow;
}

void main()
{
    // === 1. Determine Terrain Color by Height ===
    float height = v_FragPos.y;
    vec3 terrainColor;
    float grassFactor = smoothstep(u_sandLevel - u_blendRange, u_sandLevel + u_blendRange, height);
    terrainColor = mix(u_sandColor, u_grassColor, grassFactor);
    float rockFactor = smoothstep(u_grassLevel - u_blendRange, u_grassLevel + u_blendRange, height);
    terrainColor = mix(terrainColor, u_rockColor, rockFactor);
    float snowFactor = smoothstep(u_rockLevel - u_blendRange, u_rockLevel + u_blendRange, height);
    terrainColor = mix(terrainColor, u_snowColor, snowFactor);
    float noise = hash(v_FragPos.xz * 0.1) * 0.1 - 0.05;
    terrainColor += noise;

    // === 1.5 APPLY WETNESS EFFECT ===
    vec3 albedo = terrainColor;
    float roughness = 0.8; // Base roughness for dry terrain

    // As wetness increases, darken the albedo and decrease roughness (making it shinier)
    albedo = mix(albedo, albedo * 0.5, u_Wetness);
    roughness = mix(roughness, 0.2, u_Wetness);

    // === 2. NEW: Day & Night Lighting Calculation ===
    vec3 lightDir = normalize(sunDirection.y < 0.0 ? -sunDirection : sunDirection);
    
    // Define our light colors
    vec3 sunColor = vec3(1.0, 0.95, 0.85);
    vec3 sunriseColor = vec3(1.0, 0.6, 0.3);
    vec3 moonColor = vec3(0.4, 0.5, 0.65); // A cool, dim moonlight

    vec3 lightColor;
    float ambientStrength;
    float specularStrength;

    // Your finding: "sun is below ground at day", so sunDirection.y is negative during the day.
    float celestialY = sunDirection.y;

    if (celestialY < -0.1) { 
        // DAYTIME: The sun is "below" the horizon (in your coordinate system)
        lightColor = sunColor;
        ambientStrength = 0.5;
        specularStrength = 0.6;
    } 
    else if (celestialY > 0.1) { 
        // NIGHTTIME: The celestial object (moon) is in the sky
        lightColor = moonColor;
        ambientStrength = 0.15;
        specularStrength = 0.1; // Less specular from moonlight
    } 
    else { 
        // DAWN / DUSK: A smooth transition period
        float t = (celestialY + 0.1) / 0.2; // Map [-0.1, 0.1] range to [0, 1]
        lightColor = mix(sunriseColor, moonColor, t);
        ambientStrength = mix(0.35, 0.15, t);
        specularStrength = mix(0.4, 0.1, t);
    }

    // In Blinn-Phong, a lower roughness translates to a higher shininess exponent
    float shininess = (1.0 - roughness) * 256.0;

    // Standard Blinn-Phong lighting calculation
    vec3 ambient = ambientStrength * lightColor * albedo;
    
    vec3 norm = normalize(v_Normal);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor * albedo;

    vec3 viewDir = normalize(viewPos - v_FragPos);
    vec3 halfDir = normalize(lightDir + viewDir); // Use Blinn-Phong for better control
    float spec = pow(max(dot(norm, halfDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * lightColor * (0.04 + 0.96 * u_Wetness); // More specular when wet
    
    float shadow = ShadowCalculation(v_FragPosLightSpace, norm);
    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular));
    
    // === 3. Calculate Fog ===
    float distance = length(viewPos - v_FragPos);
    float fogStart = 450.0;
    float fogEnd = 900.0;
    float fogFactor = clamp((fogEnd - distance) / (fogEnd - fogStart), 0.0, 1.0);
    
    vec3 result = mix(fogColor, lighting, fogFactor);
    
    FragColor = vec4(result, 1.0);
}