#version 450 core

#ifdef LUMIN_DIAG_IDS
layout (location = 6) out uvec4 gDiagnosticIds;
uniform uvec4 u_diagnosticIds;
#endif
#include "far_world_position.glsl"

// PHEROMONE TRAIL GROUND DECAL — render-only deferred decal.
// Reconstructs world position from the deferred G-buffer (gPosition = VIEW-SPACE,
// RGB16F at COLOR_ATTACHMENT0), projects world XZ into the ScentField grid, samples
// the one-way sim->render scent mirror (RG16F: R = food-trail ch2, G = home-trail
// ch3), and ADDITIVELY tints the ground albedo (COLOR_ATTACHMENT2) so the trail is
// then lit + AO-darkened by the existing deferred lighting. Determinism-neutral: the
// mirror is written from a const sim read and is NEVER read back by the sim.
//
// Host blend state (set by RenderPipeline before the draw):
//   glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
// Bound to COLOR_ATTACHMENT2 (albedo) ONLY. FragColor.a carries trail strength.

layout(location = 0) out vec4 FragColor;

uniform sampler2D gDepth;
uniform sampler2D gPosition;      // unit 0: VIEW-SPACE position (RGB16F)
uniform sampler2D u_scentField;   // unit 1: RG16F (R=food, G=home)

uniform mat4  u_inverseView;          // inverse(camera.view) -> world from view
uniform vec2  u_scentWorldOrigin;     // world XZ of grid cell (0,0) corner
uniform float u_scentInvWorldSpan;    // 1.0 / (cells * cell_size)
uniform float u_scentScale;           // raw Sample() -> intensity scale (tuned host-side)

in vec2 TexCoords;

// Amber food trail, cool teal home trail.
const vec3 FOOD_COLOR = vec3(0.95, 0.62, 0.18);
const vec3 HOME_COLOR = vec3(0.25, 0.55, 0.85);

void main() {
    vec3 viewPos = vec3(0.0);
#ifdef LUMIN_DIAG_IDS
    if (!farWorldPosition(gPosition, gDepth, TexCoords, viewPos)) { discard; }
#else
    if (!farWorldPosition(gPosition, gDepth, TexCoords, viewPos)) { FragColor = vec4(0.0); return; }
#endif
    // Empty/sky G-buffer pixels are (0,0,0) in view space; reject them.
#ifdef LUMIN_DIAG_IDS
    if (dot(viewPos, viewPos) < 1e-8) { discard; }
#else
    if (dot(viewPos, viewPos) < 1e-8) { FragColor = vec4(0.0); return; }
#endif

    vec3 worldPos = vec3(u_inverseView * vec4(viewPos, 1.0));

    // Project world XZ -> [0,1] grid UV; outside the field contributes nothing.
    vec2 uv = (worldPos.xz - u_scentWorldOrigin) * u_scentInvWorldSpan;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
#ifdef LUMIN_DIAG_IDS
        discard;
#else
        FragColor = vec4(0.0);
        return;
#endif
    }

    vec2 scent = texture(u_scentField, uv).rg;   // R=food, G=home
    // The food trail (laden ants, nest<-food) is the crisp hero path -> full weight.
    // The home trail (outbound ants) spreads broadly near the nest -> down-weight it so
    // it reads as a faint halo, not a blob that swamps the amber path.
    float foodI = clamp(scent.r * u_scentScale, 0.0, 1.0);
    float homeI = clamp(scent.g * u_scentScale * 0.55, 0.0, 1.0);

    vec3  tint = FOOD_COLOR * foodI + HOME_COLOR * homeI;
    float intensity = max(foodI, homeI * 0.7);
#ifdef LUMIN_DIAG_IDS
    if (intensity <= 0.0025) { discard; }
#else
    if (intensity <= 0.0025) { FragColor = vec4(0.0); return; }
#endif

    // glBlendFunc(SRC_ALPHA, ONE): rgb scaled by alpha, so carry strength in.a.
    FragColor = vec4(tint, intensity);
#ifdef LUMIN_DIAG_IDS
    gDiagnosticIds = u_diagnosticIds;
#endif
}
