#pragma once

// shared seeded deterministic PRNG for the SIM path (evolution genome
// mutation, future stochastic ecology). The engine already uses a splitmix64 hash
// for render-only foliage placement (FoliagePass), but that is local and render-
// side; sim code that feeds world_hash needs a reproducible generator that obeys
// the determinism contract:
//   * NO wall-clock / global RNG — every stream is a pure function of an explicit
//     integer seed, so run==replay holds and a replay reproduces the exact draws.
//   * NO libm transcendentals (log/cos/sin) — the Gaussian uses an Irwin-Hall
//     (sum-of-uniforms) approximation so it stays SimDeterminismLint-clean.
// Seed streams from (offset, id, generation,...) via splitmix64 mixing; pick a
// distinct world_seed offset per subsystem (wind+11, weather+12/13, aether+14 are
// taken; evolution and scent use distinct subsequent offsets.

#include <cstdint>
#include <string_view>

namespace luminumbra::core {

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed)
        : m_state(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

    // Compose a stream seed from up to three integer inputs (e.g. a subsystem
    // offset, an entity id, a generation/tick). Pure + order-sensitive.
    [[nodiscard]] static DeterministicRng
    seeded(std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0) {
        return DeterministicRng(mix(mix(mix(a) ^ b) ^ c));
    }

    // Named module stream: FNV-1a-64 over the domain, world seed, length-prefixed
    // name bytes, entity id and tick, followed by splitmix64. Integers (including
    // byte lengths) are encoded as eight little-endian bytes. Names are exact
    // bytes, including embedded NULs; no normalization is performed.
    // No allocation; work is linear in the two name lengths.
    [[nodiscard]] static DeterministicRng for_module(std::uint64_t world_seed,
                                                     std::string_view mod_id,
                                                     std::string_view stream_name,
                                                     std::uint64_t entity_id,
                                                     std::uint64_t tick) {
        std::uint64_t hash = 14695981039346656037ull;
        const auto append_byte = [&hash](unsigned char byte) {
            hash = (hash ^ byte) * 1099511628211ull;
        };
        const auto append_u64 = [&append_byte](std::uint64_t value) {
            for (unsigned int shift = 0; shift < 64; shift += 8) {
                append_byte(static_cast<unsigned char>(value >> shift));
            }
        };
        const auto append_name = [&append_byte, &append_u64](std::string_view name) {
            append_u64(static_cast<std::uint64_t>(name.size()));
            for (const char byte : name) {
                append_byte(static_cast<unsigned char>(byte));
            }
        };
        for (const char byte : std::string_view("luminumbra:rng:module:v1")) {
            append_byte(static_cast<unsigned char>(byte));
        }
        append_u64(world_seed);
        append_name(mod_id);
        append_name(stream_name);
        append_u64(entity_id);
        append_u64(tick);
        return DeterministicRng(mix(hash));
    }

    // splitmix64 finalizer (Steele et al. 2014) — the engine's existing render-side
    // mixing constant, promoted here as the canonical sim hash/stream stepper.
    [[nodiscard]] static std::uint64_t mix(std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    // Next 64-bit value (advances the stream deterministically).
    std::uint64_t next_u64() {
        m_state += 0x9E3779B97F4A7C15ull;
        return mix(m_state);
    }

    // Uniform float in [0, 1). 24-bit mantissa resolution; exact + portable
    // (integer -> float division, no transcendentals).
    float next_unit() {
        const std::uint32_t bits = static_cast<std::uint32_t>(next_u64() >> 40); // top 24 bits
        return static_cast<float>(bits) * (1.0f / 16777216.0f);                  // / 2^24
    }

    // Uniform float in [lo, hi).
    float next_range(float lo, float hi) {
        return lo + (hi - lo) * next_unit();
    }

    // Approximately-standard-normal sample, N(0,1), via the Irwin-Hall sum of 12
    // uniforms minus 6 (mean 0, variance 1). Libm-free and deterministic — the
    // sanctioned mutation noise on the sim path. Tails are bounded to +/-6 sigma
    // (fine for bounded-trait mutation; not for rare-event modeling).
    float next_gaussian() {
        float sum = 0.0f;
        for (int i = 0; i < 12; ++i)
            sum += next_unit();
        return sum - 6.0f;
    }

private:
    std::uint64_t m_state;
};

} // namespace luminumbra::core
