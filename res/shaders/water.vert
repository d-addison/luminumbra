#version 450 core

layout (location = 0) in vec3 a_pos;
layout (location = 1) in vec3 a_normal;

// Uniforms for transforming the chunk mesh
uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

// Outputs for the fragment shader
out VS_OUT {
    vec3 world_pos;
    vec3 world_normal;
} vs_out;

void main()
{
    // Position in world space (for lighting and world-based effects)
    vec4 world_pos_4 = u_model * vec4(a_pos, 1.0);
    vs_out.world_pos = world_pos_4.xyz;

    // Normal in world space, correctly transformed for non-uniform scaling
    mat3 normal_matrix = transpose(inverse(mat3(u_model)));
    vs_out.world_normal = normalize(normal_matrix * a_normal);

    // Final clip-space position
    gl_Position = u_projection * u_view * world_pos_4;
}