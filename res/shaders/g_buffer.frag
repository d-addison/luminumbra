#version 450 core
// The G-buffer has multiple output targets.
layout (location = 0) out vec4 gPosition;  // VIEW-space position
layout (location = 1) out vec4 gNormal;    // VIEW-space normal
layout (location = 2) out vec4 gAlbedo;    // Base color (diffuse)
layout (location = 3) out vec4 gMaterial;  // R: Metallic, G: Roughness, B: AO, A: MaterialID

// Input from the vertex shader, with "flat" interpolation for the integer ID
in VS_OUT {
    vec3 FragPos; // Now in VIEW SPACE
    vec3 Normal;  // Now in VIEW SPACE
    flat uint MaterialID;
} fs_in;

void main()
{
    // Store the fragment's view-space position.
    gPosition = vec4(fs_in.FragPos, 1.0);
    // Store the normalized view-space normal.
    gNormal = vec4(normalize(fs_in.Normal), 1.0);

    // --- Determine material properties based on the Material ID ---
    // (This part of the code is unchanged)
    vec3 albedo = vec3(0.7); // Default color
    float metallic = 0.1;
    float roughness = 0.8;
    float ao = 1.0; // Ambient Occlusion, default to fully lit. SSAO will modify this later.

    if (fs_in.MaterialID == 1u) { // Stone
        albedo = vec3(0.5);
        metallic = 0.05;
        roughness = 0.85;
    } else if (fs_in.MaterialID == 2u) { // Soil
        albedo = vec3(0.3, 0.15, 0.05);
        metallic = 0.0;
        roughness = 0.9;
    } else if (fs_in.MaterialID == 3u) { // Grass
        albedo = vec3(0.2, 0.6, 0.15);
        metallic = 0.0;
        roughness = 0.8;
    } else if (fs_in.MaterialID == 4u) { // Sand
        albedo = vec3(0.9, 0.8, 0.5);
        metallic = 0.0;
        roughness = 0.75;
    } else if (fs_in.MaterialID == 6u) { // Example: Luminous Crystal
        albedo = vec3(0.9, 0.9, 1.0);
        metallic = 0.2;
        roughness = 0.15;
    }

    // Output the determined properties to the G-buffer textures.
    gAlbedo = vec4(albedo, 1.0);
    gMaterial.r = metallic;
    gMaterial.g = roughness;
    gMaterial.b = ao;
    // Pack the MaterialID into the alpha channel, normalized to the [0, 1] range.
    gMaterial.a = float(fs_in.MaterialID) / 255.0;
}