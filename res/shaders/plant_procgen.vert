#version 450 core
//  render-only PROCEDURAL plant pass (behind render.plant_procgen).
// Vertices arrive PRE-TRANSFORMED into WORLD space (the CPU bake in
// PlantProcgenPass transforms each plant's local mesh to its world position),
// so there is no per-instance model matrix here — the model transform is
// identity. The pass writes the SAME deferred G-buffer outputs the static-mesh
// path does (view-space position + view-space octahedral normal + albedo).
layout (location = 0) in vec3 aPos;     // WORLD-space position
layout (location = 1) in vec3 aNormal;  // WORLD-space normal
layout (location = 2) in vec2 aUV;      // bake UV (.x: 0 = bark, 1 = leaf)
layout (location = 3) in vec3 aColor;   // per-vertex albedo (genetic + seasonal)

out VS_OUT {
    vec3 FragPos;   // VIEW space
    vec3 Normal;    // VIEW space
    vec2 UV;
    vec3 Color;     // per-vertex albedo
} vs_out;

uniform mat4 projection;
uniform mat4 view;
uniform mat3 u_normalViewMatrix; // mat3(view): world normal -> view normal
uniform float u_time = 0.0;          // render wall-clock; never feeds the sim
uniform float u_windStrength = 0.0;  // 0 = still

void main()
{
    // Render-only WIND SWAY: only the leaf cards (aUV.x == 1) sway; the woody trunk/branches
    // stay rigid. Two octaves for a natural flutter. Mirrors instanced_mesh.vert.
    vec3 worldPos = aPos;
    if (u_windStrength > 0.0) {
        float phase = worldPos.x * 0.35 + worldPos.z * 0.27;
        float sway = (sin(u_time * 1.3 + phase) * 0.12 + sin(u_time * 2.9 + phase * 1.7) * 0.05)
                     * u_windStrength * aUV.x;
        worldPos.x += sway;
        worldPos.z += sway * 0.6;
    }
    vec4 viewPos = view * vec4(worldPos, 1.0);
    vs_out.FragPos = vec3(viewPos);
    vs_out.Normal = normalize(u_normalViewMatrix * aNormal);
    vs_out.UV = aUV;
    vs_out.Color = aColor;
    gl_Position = projection * viewPos;
}
