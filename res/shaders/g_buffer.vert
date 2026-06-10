#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
// The per-vertex material ID from the VoxelVertex struct
layout (location = 2) in uint aMaterialID;

// Define the output interface block to match g_buffer.frag
out VS_OUT {
    vec3 FragPos; // Transformed to VIEW SPACE
    vec3 Normal;  // Transformed to VIEW SPACE
    flat uint MaterialID;
} vs_out;

// Uniforms for transforming the entire chunk mesh
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;

void main()
{
    // Calculate view-space position
    vec4 viewPos = view * model * vec4(aPos, 1.0);
    vs_out.FragPos = vec3(viewPos);
    vs_out.Normal = normalize(normalMatrix * aNormal);
    vs_out.MaterialID = aMaterialID;

    // Calculate the final clip-space position for rasterization
    gl_Position = projection * viewPos;
}
