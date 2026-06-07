#version 450 core
// Compressed G-buffer format - 50% memory bandwidth reduction
layout (location = 0) out vec2 gPositionDepth;     // RG32F: XZ in view space, depth reconstruction
layout (location = 1) out vec4 gNormalMaterial;    // RGB10A2: Octahedral normal + material ID  
layout (location = 2) out vec4 gAlbedoRoughness;   // RGBA8: RGB albedo + roughness
layout (location = 3) out vec2 gMetallicAO;        // RG16F: Metallic + AO

// Octahedral normal encoding functions
vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
}

vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

// Material lookup texture
uniform sampler2D u_materialLUT;

// Input from the vertex shader, with "flat" interpolation for the integer ID
in VS_OUT {
    vec3 FragPos; // Now in VIEW SPACE
    vec3 Normal;  // Now in VIEW SPACE
    flat uint MaterialID;
} fs_in;

void main()
{
    if (fs_in.MaterialID == 7u) { // Water
        discard;
    }
    
    // --- Compressed G-Buffer Output ---
    
    // Store XZ components only, Y will be reconstructed from depth
    gPositionDepth = fs_in.FragPos.xz;
    
    // Encode normal using octahedral mapping and pack with material ID
    vec2 encoded_normal = encode_octahedral(normalize(fs_in.Normal));
    float material_id_normalized = float(fs_in.MaterialID) / 255.0;
    gNormalMaterial = vec4(encoded_normal * 0.5 + 0.5, 0.0, material_id_normalized);

    // --- Material properties from lookup texture ---
    float matIndex = float(fs_in.MaterialID) / 255.0;
    vec4 matProps = texture(u_materialLUT, vec2(matIndex, 0.5));
    
    float metallic = matProps.r;
    float roughness = matProps.g;
    float ao = matProps.b;
    
    // Default albedo colors (can be replaced with tri-planar textures in lighting pass)
    vec3 albedo = vec3(0.7); 
    switch (fs_in.MaterialID) {
        case 1u: albedo = vec3(0.5); break;                    // Stone
        case 2u: albedo = vec3(0.3, 0.15, 0.05); break;       // Soil
        case 3u: albedo = vec3(0.2, 0.6, 0.15); break;        // Grass
        case 4u: albedo = vec3(0.9, 0.8, 0.5); break;         // Sand
        case 6u: albedo = vec3(0.85, 0.95, 1.0); break;       // Luminous Crystal
    }

    // Pack albedo and roughness
    gAlbedoRoughness = vec4(albedo, roughness);
    
    // Pack metallic and AO
    gMetallicAO = vec2(metallic, ao);
}