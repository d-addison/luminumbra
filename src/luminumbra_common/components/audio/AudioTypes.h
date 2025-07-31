#pragma once
#include <cstdint>
#include <string>

namespace Luminumbra::Common {

// An opaque handle to a playing sound instance, managed by a backend
using AudioEventHandle = uint64_t;

// An ID for a sound event, defined in a soundbank
using AudioEventID = std::string;

// An ID for a sound parameter (e.g., "reverb", "intensity")
using AudioParamID = std::string;

} // namespace Luminumbra::Common