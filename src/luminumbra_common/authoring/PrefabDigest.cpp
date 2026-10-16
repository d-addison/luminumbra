#include "PrefabDigest.h"

#include "../core/Sha256.h"

#include <iomanip>
#include <sstream>

namespace Luminumbra::Authoring::Detail {
std::string Sha256(std::span<const std::uint8_t> bytes) {
    const auto digest = Core::Sha256(bytes);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}
} // namespace Luminumbra::Authoring::Detail
