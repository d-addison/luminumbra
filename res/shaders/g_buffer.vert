#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
// The per-vertex material ID from the VoxelVertex struct
layout (location = 2) in uint aMaterialID;

// Define the output interface block to match g_buffer.frag
out VS_OUT {
    vec3 FragPos;      // VIEW SPACE (g-buffer position output)
    vec3 Normal;       // VIEW SPACE (octahedral-encoded into the g-buffer)
    vec3 WorldPos;     // WORLD SPACE (triplanar projection, T-I4-7)
    vec3 WorldNormal;  // WORLD SPACE (triplanar blend weights + normal mapping)
    vec2 UV;           // mesh UV (terrain has none -> 0; T-I4-8)
    flat uint MaterialID;
} vs_out;

// Uniforms for transforming the entire chunk mesh
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;

void main()
{
    // World-space position/normal for triplanar terrain sampling (T-I4-7).
    vec4 worldPos = model * vec4(aPos, 1.0);
    vs_out.WorldPos = vec3(worldPos);
    vs_out.WorldNormal = normalize(mat3(model) * aNormal);

    // Calculate view-space position
    vec4 viewPos = view * worldPos;
    vs_out.FragPos = vec3(viewPos);
    vs_out.Normal = normalize(normalMatrix * aNormal);
    vs_out.UV = vec2(0.0); // terrain uses triplanar projection, not mesh UVs
    vs_out.MaterialID = aMaterialID;

    // Calculate the final clip-space position for rasterization
    gl_Position = projection * viewPos;
}
