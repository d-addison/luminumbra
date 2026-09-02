#pragma once

// game.math: the ONE shared pure-float helper set for the game libraries.
//
// HISTORY. PhotoCamera, PhotoFilters, DifficultyProfile, LightTools, PhotoCodex,
// PhotoSession, Objectives, and PhotoMode each used to carry a private copy of the
// same [0,1] clamp under a per-file name (CameraClamp01, FilterClamp01, ...). The
// renames were a stopgap for an ODR collision between earlier same-named
// non-static copies, and they blocked the gamelibs hardening suite from compiling
// the full library set into one translation unit. Defining the helper ONCE here
// removes both the duplication and the collision by construction.
//
// DETERMINISM CONTRACT. float compares and returns only — no libm, no rng, no
// state. Bit-stable on every platform; safe for any sim-adjacent caller.

namespace luminumbra::game {

inline float Clamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

} // namespace luminumbra::game
