#include "../support/RegionFixtureCapture.h"
#include "../support/RegionFixtureCapturePublication.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Luminumbra::TestSupport::RegionFixtureWorld;
using Luminumbra::world::GameSession;

nlohmann::json Capture(bool plant) {
    RegionFixtureWorld root;
    GameSession session, loaded;
    return Luminumbra::TestSupport::CaptureRegionFixture(root, session, loaded, plant);
}

bool IsWithinSourceTree(const fs::path& path) {
    const auto root = fs::canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
    // Compare filesystem identities, including Windows case and short-name aliases.
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (fs::equivalent(ancestor, root))
            return true;
        if (ancestor == ancestor.parent_path())
            break;
    }
    return false;
}

bool RequestedCapture(fs::path& destination) {
#if defined(_MSC_VER)
    wchar_t* value = nullptr;
    std::size_t size = 0;
    const auto result = _wdupenv_s(&value, &size, L"LUMINUMBRA_REGION_FIXTURE_CAPTURE");
    const std::unique_ptr<wchar_t, decltype(&std::free)> requested(value, &std::free);
    if (result != 0)
        throw std::runtime_error("cannot read capture destination");
    if (requested == nullptr)
        return false;
    destination = fs::path(requested.get());
#elif defined(_WIN32)
    const wchar_t* requested = _wgetenv(L"LUMINUMBRA_REGION_FIXTURE_CAPTURE");
    if (requested == nullptr)
        return false;
    destination = fs::path(requested);
#else
    const char* requested = std::getenv("LUMINUMBRA_REGION_FIXTURE_CAPTURE");
    if (requested == nullptr)
        return false;
    destination = Luminumbra::Filesystem::PathFromUtf8(requested);
#endif
    return true;
}

void EmitCapture() {
    fs::path destination;
    if (!RequestedCapture(destination)) {
        throw std::runtime_error("set LUMINUMBRA_REGION_FIXTURE_CAPTURE to request a capture");
    }
    if (!Luminumbra::Filesystem::IsAbsolutePath(destination)) {
        throw std::runtime_error("capture destination must be absolute");
    }
    const auto parent = fs::canonical(destination.parent_path());
    destination = parent / destination.filename();
    if (IsWithinSourceTree(parent)) {
        throw std::runtime_error("capture destination must be outside source tree");
    }
    if (fs::exists(destination)) {
        throw std::runtime_error("capture destination must be new");
    }
    const nlohmann::json capture{
        {"schema", "luminumbra.region_fixture_capture.v1"},
        {"scenarios", {{"devel-off", Capture(false)}, {"devel-off-plant-edit", Capture(true)}}}};
    const auto payload = capture.dump() + '\n';
    // Cap total capture input at 16 MiB before staging any output.
    if (payload.size() > (16U << 20)) {
        throw std::runtime_error("capture payload exceeds size limit");
    }
    Luminumbra::TestSupport::PublishCapture(destination, payload);
}
} // namespace

int main() {
    try {
        EmitCapture();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
