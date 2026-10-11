#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace Luminumbra::Authoring::Detail {
std::string Sha256(std::span<const std::uint8_t> bytes);
}
