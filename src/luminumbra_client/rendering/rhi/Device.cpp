#include "rendering/rhi/Device.h"

// The ONLY place Diligent headers are included on the client side (rhi/ boundary).
#include "DeviceContext.h"
#include "EngineFactoryOpenGL.h"
#include "EngineFactoryVk.h"
#include "RenderDevice.h"

namespace Luminumbra::Rendering::Rhi {

namespace {

// Query a freshly-created device for its adapter/type, then tear it down. The
// immediate context is Flush'ed before release -- Diligent logs an error on
// teardown of a context with outstanding (never-submitted) commands otherwise.
DeviceBringupResult QueryAndRelease(Diligent::IRenderDevice* device,
                                    Diligent::IDeviceContext* context,
                                    Backend backend) {
    DeviceBringupResult result;
    result.backend = BackendName(backend);
    if (device == nullptr) {
        result.created = false;
        result.diagnostic = "device factory returned a null device";
        if (context != nullptr) {
            context->Flush();
            context->Release();
        }
        return result;
    }
    const Diligent::GraphicsAdapterInfo& adapter = device->GetAdapterInfo();
    const Diligent::RenderDeviceInfo& info = device->GetDeviceInfo();
    result.created = true;
    result.adapter = adapter.Description;
    result.device_type = static_cast<int>(info.Type);
    if (context != nullptr) {
        context->Flush();
        context->Release();
    }
    device->Release();
    return result;
}

} // namespace

DeviceBringupResult CreateHeadlessDevice(Backend backend) {
    using namespace Diligent;
    switch (backend) {
        case Backend::Gl: {
            IEngineFactoryOpenGL* factory = GetEngineFactoryOpenGL();
            EngineGLCreateInfo create_info;
            IRenderDevice* device = nullptr;
            IDeviceContext* context = nullptr;
            // Attaches to the GL context current on this thread (headless: the
            // caller stands one up first). No window/swapchain is created.
            factory->AttachToActiveGLContext(create_info, &device, &context);
            return QueryAndRelease(device, context, backend);
        }
        case Backend::Vulkan: {
            IEngineFactoryVk* factory = GetEngineFactoryVk();
            EngineVkCreateInfo create_info;
            IRenderDevice* device = nullptr;
            IDeviceContext* context = nullptr;
            // Creates its own Vulkan instance/device with no swapchain -- the
            // natural headless shape (device-no-swapchain).
            factory->CreateDeviceAndContextsVk(create_info, &device, &context);
            return QueryAndRelease(device, context, backend);
        }
        default: {
            DeviceBringupResult result;
            result.backend = BackendName(backend);
            result.created = false;
            result.diagnostic = "unsupported RHI backend";
            return result;
        }
    }
}

} // namespace Luminumbra::Rendering::Rhi
