#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace Luminumbra::Systems {

// Internal, opt-in test observer. Installed only by AmbientDiagnosticAccess on
// the test thread before construction; ordinary updates have a null observer.
// Copy existing values only: no simulation math or vendor instrumentation here.
class AmbientDiagnosticCapture {
public:
    virtual ~AmbientDiagnosticCapture() = default;
    [[nodiscard]] virtual bool WantsTick(std::uint64_t tick) const = 0;
    virtual void
    Floats(std::uint64_t tick, std::string_view stage, const float* values, std::size_t count) = 0;
    virtual void
    Sweep(std::uint64_t tick, int iteration, const float* values, std::size_t count) = 0;

    void Samples(std::uint64_t tick, std::string_view stage, std::initializer_list<float> values) {
        Floats(tick, stage, values.begin(), values.size());
    }
};

} // namespace Luminumbra::Systems
