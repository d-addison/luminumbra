#pragma once

// spec 021 GPU-P02 / spec 014 FR-B.1: headless RHI device bring-up.
//
// This header exposes device creation WITHOUT any Diligent type escaping rhi/
// (the "no Diligent header escapes rhi/" rule that GPU-P03's RhiNoReexport gate
// enforces): the result carries only std types and a plain int for the backend's
// device-type tag. Only Device.cpp includes Diligent, and only the
// RhiDeviceBringupGpu ctest links it in P02 -- no shipping binary does.

#include "rendering/rhi/RhiBackend.h"

#include <string>

namespace Luminumbra::Rendering::Rhi {

struct DeviceBringupResult {
    bool created = false;      // a live device was produced
    std::string backend;       // BackendName of the requested backend
    std::string adapter;       // GPU/adapter description (when created)
    int device_type = -1;      // Diligent RENDER_DEVICE_TYPE as an opaque int (no enum leaks)
    std::string diagnostic;    // failure/context detail (empty on success)
};

// Create a headless RHI device for `backend`, query its adapter, then release it
// (bring-up only -- P02 ports no pass, so no device is retained). For Backend::Gl
// a current GL context MUST already exist on the calling thread (Diligent attaches
// to the active context). Backend::Dx12 is parsed but its device creation is a
// later phase, so it returns created=false with an honest diagnostic.
DeviceBringupResult CreateHeadlessDevice(Backend backend);

}  // namespace Luminumbra::Rendering::Rhi
