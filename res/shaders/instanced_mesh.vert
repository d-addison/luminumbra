#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV; // I8: mesh UV for the static-model texture lane

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
// I8 wind (render-only): u_windStrength is per-draw-group (trees > 0, rigid props 0);
// u_time animates the sway. Render wall-clock; never feeds the sim.
uniform float u_time = 0.0;
uniform float u_windStrength = 0.0;

void main()
{
    // World-space position for triplanar sampling + wind.
    vec4 worldPos = aInstanceMatrix * vec4(aPos, 1.0);

    // I8 wind sway: upper geometry sways, the base stays planted (amount scales
    // with local height aPos.y). Phase varies by world position so neighbouring
    // trees don't sway in unison; two octaves give a natural gust + flutter.
    if (u_windStrength > 0.0) {
        float swayHeight = max(aPos.y, 0.0);
        float phase = worldPos.x * 0.15 + worldPos.z * 0.13;
        float sway = (sin(u_time * 1.3 + phase) * 0.06 +
                      sin(u_time * 2.7 + phase * 1.7) * 0.025) * swayHeight * u_windStrength;
        worldPos.x += sway;
        worldPos.z += sway * 0.6;
    }

    vs_out.WorldPos = vec3(worldPos);
    vs_out.WorldNormal = normalize(mat3(aInstanceMatrix) * aNormal);

    // View-space position from the (wind-displaced) world position.
    vec4 viewPos = view * worldPos;
    vs_out.FragPos = vec3(viewPos);

    mat3 normalMatrix = mat3(transpose(inverse(view * aInstanceMatrix)));
    vs_out.Normal = normalize(normalMatrix * aNormal); // Also includes the fix from last time
    vs_out.UV = aUV; // I8: pass the mesh UV so the static-model texture lane can sample it

    vs_out.MaterialID = uint(u_materialId);

    gl_Position = projection * viewPos;
}