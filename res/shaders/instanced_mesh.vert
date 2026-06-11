#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
// location 2 is unused

// Instanced model matrix (each mat4 is 4 vec4s)
layout (location = 3) in mat4 aInstanceMatrix;

// Output interface block
out VS_OUT {
    vec3 FragPos; // Transformed to VIEW SPACE
    vec3 Normal;  // Transformed to VIEW SPACE
    flat uint MaterialID;
} vs_out;

uniform mat4 projection;
uniform mat4 view;
// T-I3-16: per-draw-group material id from StaticMeshComponent (the previous
// hardcoded MaterialID = 3u painted every static mesh as grass).
uniform int u_materialId;

void main()
{
    // Combine view and instance matrices
    mat4 viewModel = view * aInstanceMatrix;

    // Calculate view-space position using the combined matrix
    vec4 viewPos = viewModel * vec4(aPos, 1.0);
    vs_out.FragPos = vec3(viewPos);

    // This is the correct place to use 'viewModel'
    mat3 normalMatrix = mat3(transpose(inverse(viewModel)));
    vs_out.Normal = normalize(normalMatrix * aNormal); // Also includes the fix from last time

    vs_out.MaterialID = uint(u_materialId);

    gl_Position = projection * viewPos;
}