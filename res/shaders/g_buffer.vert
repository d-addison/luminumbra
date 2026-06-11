#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
// The per-vertex material ID from the VoxelVertex struct
layout (location = 2) in uint aMaterialID;

// T-I4-DR-horizon-sliver-render: redeclare the built-in output block sized for
// one clip distance (some GL drivers ignore gl_ClipDistance writes otherwise).
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};

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

// T-I4-DR-horizon-sliver-render: far-region geometry clip band (meters). Set > 0
// only for far draws (live/static/skinned keep them 0 -> clip inert).
//   u_farClipNearRadius: clip far geometry CLOSER than this (radial). Removes
//     the camera-straddling near triangles (one vertex behind the near plane,
//     whose perspective divide blew up and the near-plane clip drew as a streak).
//   u_farClipFarRadius: clip far geometry FARTHER than this (radial). Removes the
//     far/edge triangles near the camera far plane (1000 m) and frustum corner
//     that rasterized as the thick horizon streak. Both bands are invisible
//     (inside the live ring / past the far plane), so nothing visible is lost.
uniform float u_farClipNearRadius;
uniform float u_farClipFarRadius;

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

    // Far-region radial clip band: positive only between the near and far radii.
    // gl_ClipDistance[0] < 0 -> the GL clips the primitive before rasterization.
    if (u_farClipNearRadius > 0.0 || u_farClipFarRadius > 0.0) {
        float d = length(viewPos.xyz);
        gl_ClipDistance[0] = min(d - u_farClipNearRadius, u_farClipFarRadius - d);
    } else {
        gl_ClipDistance[0] = 1.0;
    }

    // Calculate the final clip-space position for rasterization
    gl_Position = projection * viewPos;
}
