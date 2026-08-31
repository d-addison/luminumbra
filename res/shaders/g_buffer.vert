#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
// The per-vertex material ID from the VoxelVertex struct
layout (location = 2) in uint aMaterialID;
// per-DRAW chunk world origin (instanced attribute, divisor 1). Live
// terrain is submitted with glMultiDrawElementsIndirect from the shared
// geometry pool; each draw's baseInstance selects this origin, replacing the
// per-draw 'model' uniform. Only consumed when u_useInstanceOrigin == 1.
layout (location = 3) in vec3 aOrigin;

// redeclare the built-in output block sized for
// one clip distance (some GL drivers ignore gl_ClipDistance writes otherwise).
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};

// Define the output interface block to match g_buffer.frag
out VS_OUT {
    vec3 FragPos;      // VIEW SPACE (g-buffer position output)
    vec3 Normal;       // VIEW SPACE (octahedral-encoded into the g-buffer)
    vec3 WorldPos;     // WORLD SPACE (triplanar projection, )
    vec3 WorldNormal;  // WORLD SPACE (triplanar blend weights + normal mapping)
    vec2 UV;           // mesh UV (terrain has none -> 0; )
    flat uint MaterialID;
    vec3 Tint;         // per-instance tint (terrain: white = no-op, matches g_buffer.frag VS_OUT)
    vec3 PrevWorldPos; //  TAAU: terrain is static -> == WorldPos (camera-only reprojection)
} vs_out;

// Uniforms for transforming the entire chunk mesh
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;
// world->view normal rotation for the instanced live-terrain path
// (== mat3(view); the pass already sets this). The camera view is rigid, so this
// equals the legacy transpose(inverse(mat3(view*model))) for translation-only
// chunk models.
uniform mat3 u_normalViewMatrix;
// when 1, the model transform is a pure translation by aOrigin (live
// terrain pool draw) and the world->view normal uses u_normalViewMatrix
// (== mat3(view)); when 0 the legacy per-draw 'model' / 'normalMatrix' uniforms
// drive it (far-LOD region draws, which set those uniforms per region).
uniform int u_useInstanceOrigin;

// far-region geometry clip band (meters). Set > 0
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
    // World-space position/normal for triplanar terrain sampling.
    // the live-terrain pool path applies a pure translation by the
    // per-draw chunk origin (mat3(model) is identity, so the world normal is
    // the mesh normal); the world->view normal uses u_normalViewMatrix, which
    // equals mat3(view) and -- because the camera view is a rigid (orthonormal)
    // transform -- is identical to the legacy transpose(inverse(mat3(view*model)))
    // when model is translation-only. Far-LOD draws keep the uniform path.
    vec3 worldPos3;
    vec3 worldNormal;
    vec3 viewNormal;
    if (u_useInstanceOrigin == 1) {
        worldPos3 = aPos + aOrigin;
        worldNormal = normalize(aNormal);
        viewNormal = normalize(u_normalViewMatrix * aNormal);
    } else {
        worldPos3 = vec3(model * vec4(aPos, 1.0));
        worldNormal = normalize(mat3(model) * aNormal);
        viewNormal = normalize(normalMatrix * aNormal);
    }
    vec4 worldPos = vec4(worldPos3, 1.0);
    vs_out.WorldPos = worldPos3;
    vs_out.PrevWorldPos = worldPos3; // Terrain does not move in world space.
    vs_out.WorldNormal = worldNormal;

    // Calculate view-space position
    vec4 viewPos = view * worldPos;
    vs_out.FragPos = vec3(viewPos);
    vs_out.Normal = viewNormal;
    vs_out.UV = vec2(0.0); // terrain uses triplanar projection, not mesh UVs
    vs_out.MaterialID = aMaterialID;
    vs_out.Tint = vec3(1.0);  // terrain is never per-instance tinted

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
