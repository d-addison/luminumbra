#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
// location 2 is unused

// Instanced model matrix (each mat4 is 4 vec4s)
layout (location = 3) in mat4 aInstanceMatrix;

// Output interface block (must match g_buffer.frag VS_OUT, T-I4-7).
out VS_OUT {
    vec3 FragPos;      // VIEW SPACE
    vec3 Normal;       // VIEW SPACE
    vec3 WorldPos;     // WORLD SPACE (triplanar projection)
    vec3 WorldNormal;  // WORLD SPACE
    vec2 UV;           // mesh UV (unused for instanced terrain props; T-I4-8)
    flat uint MaterialID;
} vs_out;

uniform mat4 projection;
uniform mat4 view;
// T-I3-16: per-draw-group material id from StaticMeshComponent (the previous
// hardcoded MaterialID = 3u painted every static mesh as grass).
uniform int u_materialId;

void main()
{
    // World-space position/normal for triplanar terrain sampling (T-I4-7).
    vec4 worldPos = aInstanceMatrix * vec4(aPos, 1.0);
    vs_out.WorldPos = vec3(worldPos);
    vs_out.WorldNormal = normalize(mat3(aInstanceMatrix) * aNormal);

    // Combine view and instance matrices
    mat4 viewModel = view * aInstanceMatrix;

    // Calculate view-space position using the combined matrix
    vec4 viewPos = viewModel * vec4(aPos, 1.0);
    vs_out.FragPos = vec3(viewPos);

    // This is the correct place to use 'viewModel'
    mat3 normalMatrix = mat3(transpose(inverse(viewModel)));
    vs_out.Normal = normalize(normalMatrix * aNormal); // Also includes the fix from last time
    vs_out.UV = vec2(0.0);

    vs_out.MaterialID = uint(u_materialId);

    gl_Position = projection * viewPos;
}