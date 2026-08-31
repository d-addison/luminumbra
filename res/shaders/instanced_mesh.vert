#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV; // Mesh UV for the static-model texture lane.

// Instanced model matrix (each mat4 is 4 vec4s)
layout (location = 3) in mat4 aInstanceMatrix;
// Per-instance albedo TINT (vast-forest: genetic green/brown variation per tree). White for
// instances that don't carry one, so the multiply is a no-op for non-tinted static meshes.
layout (location = 7) in vec3 aTint;

// Output interface block (must match g_buffer.frag VS_OUT, ).
out VS_OUT {
    vec3 FragPos;      // VIEW SPACE
    vec3 Normal;       // VIEW SPACE
    vec3 WorldPos;     // WORLD SPACE (triplanar projection)
    vec3 WorldNormal;  // WORLD SPACE
    vec2 UV;           // mesh UV (unused for instanced terrain props; )
    flat uint MaterialID;
    vec3 Tint;         // per-instance albedo tint (1,1,1 = no-op)
    vec3 PrevWorldPos; //  TAAU: world pos with PREVIOUS-frame wind sway
} vs_out;

uniform mat4 projection;
uniform mat4 view;
// per-draw-group material id from StaticMeshComponent (the previous
// hardcoded MaterialID = 3u painted every static mesh as grass).
uniform int u_materialId;
//  wind (render-only): u_windStrength is per-draw-group (trees > 0, rigid props 0);
// u_time animates the sway. Render wall-clock; never feeds the sim.
uniform float u_time = 0.0;
uniform float u_prevTime = 0.0;   //  TAAU: previous frame's wind clock (for motion vectors)
uniform float u_windStrength = 0.0;

// the wind sway displacement at a given wall-clock time. Kept as a function so the
// current and previous frames apply byte-identical math (only the time differs) — the
// motion vector is then exactly the per-frame sway delta, with no drift from a mismatch.
vec3 windSway(vec3 base, float t)
{
    if (u_windStrength <= 0.0) return base;
    float swayHeight = max(aPos.y, 0.0);
    float phase = base.x * 0.15 + base.z * 0.13;
    float sway = (sin(t * 1.3 + phase) * 0.06 +
                  sin(t * 2.7 + phase * 1.7) * 0.025) * swayHeight * u_windStrength;
    base.x += sway;
    base.z += sway * 0.6;
    return base;
}

void main()
{
    // World-space position for triplanar sampling + wind.
    vec4 worldPos = aInstanceMatrix * vec4(aPos, 1.0);

    //  wind sway: upper geometry sways, the base stays planted (amount scales
    // with local height aPos.y). Phase varies by world position so neighbouring
    // trees don't sway in unison; two octaves give a natural gust + flutter.
    // phase uses the UNDISPLACED world x/z (same both frames) so the only
    // delta between current and previous is the time argument.
    vec3 undisplaced = vec3(worldPos);
    worldPos = vec4(windSway(undisplaced, u_time), 1.0);
    vs_out.PrevWorldPos = windSway(undisplaced, u_prevTime);

    vs_out.WorldPos = vec3(worldPos);
    vs_out.WorldNormal = normalize(mat3(aInstanceMatrix) * aNormal);

    // View-space position from the (wind-displaced) world position.
    vec4 viewPos = view * worldPos;
    vs_out.FragPos = vec3(viewPos);

    mat3 normalMatrix = mat3(transpose(inverse(view * aInstanceMatrix)));
    vs_out.Normal = normalize(normalMatrix * aNormal); // Also includes the fix from last time
    vs_out.UV = aUV; // Pass UVs to the static-model texture lane.

    vs_out.MaterialID = uint(u_materialId);
    vs_out.Tint = aTint;

    gl_Position = projection * viewPos;
}
