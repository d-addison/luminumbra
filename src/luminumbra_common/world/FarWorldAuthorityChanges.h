#pragma once

#include "luminumbra/core/Types.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Luminumbra {
class Chunk;
namespace World {
struct FarWorldAuthorityState;
enum class FarWorldAuthorityChange : std::uint8_t {
    Terrain,
    Water,
    Membership
};

// Typed render consumer; independent of simulation/save observers. Membership
// and binding run at the owner publication boundary; dirty notifications may
// come from workers. Storage is bounded to 512 pending notifications.
class FarWorldAuthorityChanges final
    : public std::enable_shared_from_this<FarWorldAuthorityChanges> {
public:
    explicit FarWorldAuthorityChanges(std::shared_ptr<FarWorldAuthorityState> state);
    void notify(const IVec3& coords, FarWorldAuthorityChange change);
    void membership(const std::shared_ptr<Chunk>& chunk, bool parked, bool present);
    void clear();
    std::size_t record_limit() const;

private:
    std::shared_ptr<FarWorldAuthorityState> state_;
};
} // namespace World
} // namespace Luminumbra
