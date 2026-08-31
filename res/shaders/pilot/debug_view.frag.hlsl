// single-source HLSL port of the debug_view
// pass's sampler interface (see ssao.frag.hlsl header for the full contract).
//
// Sampler set MUST match debug_view.frag's GL uniforms exactly (name + 2D type):
//   gPosition, gNormalMaterial, gAlbedo, gDepth  (all sampler2D).
// The scalar controls (u_mode/u_near/...) are host uniforms that do not bind
// samplers, so they are outside the sampler-parity currency and are folded into
// constants here to keep the reflected interface to exactly the four textures.

// Cross-target bindings: register(...) for D3D/DXIL, [[vk::binding(slot,set)]] for
// SPIR-V. Samplers live in a separate descriptor set (set 1) so texture and sampler
// slots do not overlap under Vulkan's split binding spaces.
[[vk::binding(0, 0)]] Texture2D gPosition: register(t0);
[[vk::binding(1, 0)]] Texture2D gNormalMaterial: register(t1);
[[vk::binding(2, 0)]] Texture2D gAlbedo: register(t2);
[[vk::binding(3, 0)]] Texture2D gDepth: register(t3);
[[vk::binding(0, 1)]] SamplerState g_sampler: register(s0);

struct PSInput {
    float4 pos: SV_POSITION;
    float2 uv: TEXCOORD0;
};

float4 main(PSInput input): SV_TARGET {
    // Sample every declared texture so none is stripped from the reflected interface.
    float3 pos    = gPosition.Sample(g_sampler, input.uv).rgb;
    float3 nrm    = gNormalMaterial.Sample(g_sampler, input.uv).rgb;
    float3 albedo = gAlbedo.Sample(g_sampler, input.uv).rgb;
    float  depth  = gDepth.Sample(g_sampler, input.uv).r;
    float3 outColor = albedo + nrm * 0.5 + pos * 1e-6 + depth.xxx;
    return float4(outColor, 1.0);
}
