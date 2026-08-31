//   /   (Group F pulled early for the pilot pair):
// single-source HLSL port of the ssao pass's sampler interface. Compiled AND reflected
// by a single slangc invocation (-target spirv -reflection-json); its reflected layout
// is asserted equal to the GL-introspected ssao.frag layout and the declared
// ExpectedLayout (PilotShaderReflectionParity).  validates the REFLECTED interface
// only; the ported-HLSL render + FLIP-vs-golden lands with the pilot.
//
// Sampler set MUST match ssao.frag's GL uniforms exactly (name + 2D type):
//   gPosition, gNormalMaterial, u_noiseTexture  (all sampler2D).

// Cross-target bindings: register(...) for D3D/DXIL, [[vk::binding(slot,set)]] for
// SPIR-V. Samplers live in a separate descriptor set (set 1) so texture and sampler
// slots do not overlap under Vulkan's split binding spaces.
[[vk::binding(0, 0)]] Texture2D gPosition: register(t0);
[[vk::binding(1, 0)]] Texture2D gNormalMaterial: register(t1);
[[vk::binding(2, 0)]] Texture2D u_noiseTexture: register(t2);
[[vk::binding(0, 1)]] SamplerState g_sampler: register(s0);

struct PSInput {
    float4 pos: SV_POSITION;
    float2 uv: TEXCOORD0;
};

float4 main(PSInput input): SV_TARGET {
    // Sample every declared texture so the linker keeps all three in the reflected
    // interface (an unused sampler would be stripped and break the parity check).
    float3 p     = gPosition.Sample(g_sampler, input.uv).rgb;
    float3 n     = gNormalMaterial.Sample(g_sampler, input.uv).rgb;
    float  noise = u_noiseTexture.Sample(g_sampler, input.uv).r;
    float occlusion = saturate(dot(normalize(n + 1e-5), float3(0.0, 0.0, 1.0)) + noise + p.z * 1e-6);
    return float4(occlusion.xxx, 1.0);
}
