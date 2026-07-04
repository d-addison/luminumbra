#pragma once

// spec 021 GPU-P02 / spec 014 FR-B.2: the runtime graphics-backend selector.
//
// This header is the toolchain-independent seam of the RHI bring-up: it declares
// the backend enum and the LUMIN_RHI parse, with NO dependency on Diligent, GL, or
// any GPU. Device/swapchain creation lives in the backend-specific TUs behind this
// seam; nothing here pulls a Diligent header (the "no Diligent header escapes rhi/"
// rule, enforced by the RhiNoReexport gate in GPU-P03).
//
// GL is the default and the reference/ship backend at every step of the migration
// (014 NFR-2); vulkan/dx12 are the migration targets. The value is read once at
// startup, mirroring the LUMINUMBRA_JOB_WORKERS idiom in ServerWorldRunner::Boot.

namespace Luminumbra::Rendering::Rhi {

enum class Backend {
    Gl,      // default; the reference backend (raw GL 4.5 today, GL-via-Diligent under the seam)
    Vulkan,  // native Vulkan migration target
    Dx12,    // native D3D12 migration target (parsed here; device creation is a later phase)
};

// Parse a LUMIN_RHI value into a Backend. A null pointer, empty string, or any
// unrecognized value maps to Backend::Gl (the safe default) so a typo never leaves
// the client without a backend. Matching is case-insensitive and accepts the
// common aliases: "gl"/"opengl", "vulkan"/"vk", "dx12"/"d3d12".
Backend ParseRhiBackend(const char* value);

// The lowercase canonical name of a backend ("gl"/"vulkan"/"dx12"), suitable for
// logging and for round-tripping through ParseRhiBackend.
const char* BackendName(Backend backend);

// Read LUMIN_RHI from the process environment and parse it (Backend::Gl if unset).
Backend SelectedBackendFromEnv();

}  // namespace Luminumbra::Rendering::Rhi
