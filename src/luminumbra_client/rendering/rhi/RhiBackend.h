#pragma once

// the runtime graphics-backend selector.
//
// This header is the toolchain-independent seam of the RHI bring-up: it declares
// the backend enum and the LUMIN_RHI parse, with NO dependency on Diligent, GL, or
// any GPU. Device/swapchain creation lives in the backend-specific TUs behind this
// seam; nothing here pulls a Diligent header (the "no Diligent header escapes rhi/"
// rule, enforced by the RhiNoReexport gate in ).
//
// GL is the default runtime backend. Vulkan is available for the headless device
// and portability contract. The value is read once at startup.

namespace Luminumbra::Rendering::Rhi {

enum class Backend {
    Gl,     // default; the reference backend (raw GL 4.5 today, GL-via-Diligent under the seam)
    Vulkan, // headless device and portability contract
};

// Parse a LUMIN_RHI value into a Backend. A null pointer, empty string, or any
// unrecognized value maps to Backend::Gl (the safe default) so a typo never leaves
// the client without a backend. Matching is case-insensitive and accepts the
// common aliases: "gl"/"opengl" and "vulkan"/"vk".
Backend ParseRhiBackend(const char* value);

// The lowercase canonical name of a backend ("gl"/"vulkan"), suitable for
// logging and for round-tripping through ParseRhiBackend.
const char* BackendName(Backend backend);

// Read LUMIN_RHI from the process environment and parse it (Backend::Gl if unset).
Backend SelectedBackendFromEnv();

} // namespace Luminumbra::Rendering::Rhi
