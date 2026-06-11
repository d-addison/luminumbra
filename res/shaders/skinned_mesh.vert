#version 450 core
// T-I3-16: skinned mesh vertex shader (non-instanced) feeding g_buffer.frag.
//
// Vertex layout matches the .lmesh v2 (LMS2) 40-byte SkinnedVertex:
//   0 = position, 1 = normal, 2 = uv, 3 = joints (u8x4), 4 = weights
//   (u8x4, normalized so they sum to 1.0).
// The joint palette (global * inverseBind per joint, column-major) is
// computed on the CPU by the fixed-tick animation runtime (T-I3-15) and
// uploaded to the SSBO at binding 0 before each draw.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in uvec4 aJoints;
layout (location = 4) in vec4 aWeights;

layout (std430, binding = 0) readonly buffer JointPalette {
    mat4 u_jointMatrices[];
};

// Output interface block matching g_buffer.frag (T-I4-7 adds world-space
// members; T-I4-8 will route UV/textured sampling for skinned creatures).
out VS_OUT {
    vec3 FragPos;      // VIEW SPACE
    vec3 Normal;       // VIEW SPACE
    vec3 WorldPos;     // WORLD SPACE
    vec3 WorldNormal;  // WORLD SPACE
    flat uint MaterialID;
} vs_out;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
// Material comes from the entity (game data decides), not a hardcoded id.
uniform int u_materialId;

void main()
{
    mat4 skin =
        aWeights.x * u_jointMatrices[aJoints.x] +
        aWeights.y * u_jointMatrices[aJoints.y] +
        aWeights.z * u_jointMatrices[aJoints.z] +
        aWeights.w * u_jointMatrices[aJoints.w];

    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    mat3 skinNormal = mat3(skin);

    // World-space position/normal (triplanar fallback; T-I4-7 interface match).
    vec4 worldPos = model * skinnedPos;
    vs_out.WorldPos = vec3(worldPos);
    vs_out.WorldNormal = normalize(mat3(model) * (skinNormal * aNormal));

    mat4 viewModel = view * model;
    vec4 viewPos = viewModel * skinnedPos;
    vs_out.FragPos = vec3(viewPos);

    mat3 normalMatrix = mat3(transpose(inverse(viewModel)));
    vs_out.Normal = normalize(normalMatrix * (skinNormal * aNormal));

    vs_out.MaterialID = uint(u_materialId);

    gl_Position = projection * viewPos;
}
