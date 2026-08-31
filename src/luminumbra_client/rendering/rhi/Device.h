#pragma once

// headless RHI device bring-up.
//
// This header exposes device creation WITHOUT any Diligent type escaping rhi/
// (the "no Diligent header escapes rhi/" rule): the result carries only standard
// library types and a plain int for the backend's
// device-type tag. Only Device.cpp includes Diligent, and only the
// RhiDeviceBringupGpu ctest links it in  -- no shipping binary does.

#include "rendering/rhi/RhiBackend.h"

#include <string>

namespace Luminumbra::Rendering::Rhi {

struct DeviceBringupResult {
    bool created = false;   // a live device was produced
    std::string backend;    // BackendName of the requested backend
    std::string adapter;    // GPU/adapter description (when created)
    int device_type = -1;   // Diligent RENDER_DEVICE_TYPE as an opaque int (no enum leaks)
    std::string diagnostic; // failure/context detail (empty on success)
};

// Create a headless RHI device for `backend`, query its adapter, then release it
// (bring-up only, so no device is retained). For Backend::Gl
// a current GL context MUST already exist on the calling thread (Diligent attaches
// to the active context).
DeviceBringupResult CreateHeadlessDevice(Backend backend);

} // namespace Luminumbra::Rendering::Rhi
