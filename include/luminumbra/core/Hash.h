// include/luminumbra/core/Hash.h
#pragma once

#include <glm/glm.hpp>
#include <functional>

namespace std {
    template<>
    struct hash<glm::ivec3> {
        std::size_t operator()(const glm::ivec3& v) const {
            // A common hashing technique for vectors: combine hashes of components
            // using a prime number and bitwise shifts/XOR.
            std::size_t h1 = std::hash<int>()(v.x);
            std::size_t h2 = std::hash<int>()(v.y);
            std::size_t h3 = std::hash<int>()(v.z);
            // Simple combination, good enough for this purpose
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };
}