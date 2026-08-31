#pragma once
// pure mixer/ducking math for the client audio bus tree.
//
// This header is deliberately dependency-free (no miniaudio, no GL, no engine
// types) so the ducking envelope can be unit-tested CPU-only
// (test/audio/mixer_model_test.cpp) without an audio device. The concrete
// MiniaudioManager drives a MixerDucker from its per-frame Update with
// WALL-CLOCK dt: this is CLIENT/render-side audio presentation only — nothing
// here ever touches the deterministic sim or world_hash.
//
// Model: sidechain ducking. When any sound on the `events` bus is playing
// (a sting, thunder, a discovery cue), the `ambient` bus (and optionally the
// music bed) ducks toward a configured floor with linear attack smoothing,
// then releases back to unity after the last event sound ends.
//
// The envelope is expressed as a normalized "duck amount" duck01 in [0, 1]
// (0 = no duck, 1 = fully ducked); each target bus derives its own gain from
// it via DuckGain(duck01, floor). That keeps the time behaviour bus-agnostic
// and lets ambient/music use different floors off one envelope.

#include <algorithm>
#include <cstdint>

namespace Luminumbra::Client::Audio {

struct DuckParams {
    float attack_seconds = 0.05f;  // time to go 1.0 -> floor once an event starts
    float release_seconds = 0.80f; // time to go floor -> 1.0 after the last event ends
    float floor_gain = 0.5f;       // ambient-bus gain when fully ducked
    float music_floor_gain = 1.0f; // music-bed duck floor; 1.0 = music ducking DISABLED
                                   // (the default: byte-same audio until configured)
};

// Gain for a bus given a normalized duck amount and that bus's floor.
// duck01 and floor_gain are clamped to [0, 1].
constexpr float DuckGain(float duck01, float floor_gain) {
    duck01 = duck01 < 0.0f ? 0.0f : (duck01 > 1.0f ? 1.0f : duck01);
    floor_gain = floor_gain < 0.0f ? 0.0f : (floor_gain > 1.0f ? 1.0f : floor_gain);
    return 1.0f - duck01 * (1.0f - floor_gain);
}

// Closed-form envelope: the ducked gain at `seconds_since_trigger` for a single
// trigger whose event sound stays active for `active_duration_seconds`.
//   - attack phase  (t < active):  ramps linearly 1.0 -> floor over attack_seconds
//   - hold          (until the event ends): stays at the reached duck level
//   - release phase (t >= active): ramps linearly back to 1.0 over release_seconds
// Degenerate params snap: attack <= 0 ducks instantly, release <= 0 recovers
// instantly. This is the referenceable spec for the stepped MixerDucker below.
inline float DuckGainAt(float seconds_since_trigger,
                        float active_duration_seconds,
                        float attack_seconds,
                        float release_seconds,
                        float floor_gain) {
    const float t = std::max(0.0f, seconds_since_trigger);
    const float active = std::max(0.0f, active_duration_seconds);

    float duck01;
    if (t < active) {
        duck01 = (attack_seconds <= 0.0f) ? 1.0f : std::min(1.0f, t / attack_seconds);
    } else {
        const float duck_at_end =
            (attack_seconds <= 0.0f) ? 1.0f : std::min(1.0f, active / attack_seconds);
        const float since_release = t - active;
        duck01 = (release_seconds <= 0.0f)
                     ? 0.0f
                     : std::max(0.0f, duck_at_end - since_release / release_seconds);
    }
    return DuckGain(duck01, floor_gain);
}

// Stateful, dt-stepped ducker. Event-bus voices are REFERENCE-COUNTED:
// OnEventStart/OnEventEnd bracket each events-bus sound's lifetime, so
// overlapping events hold the duck until the LAST one ends, and a re-trigger
// during release simply ducks again (idempotent — no state is corrupted by
// triggering while already ducked).
class MixerDucker {
public:
    MixerDucker() = default;
    explicit MixerDucker(const DuckParams& params)
        : m_params(params) {}

    void SetParams(const DuckParams& params) {
        m_params = params;
    }
    const DuckParams& params() const {
        return m_params;
    }

    // An events-bus sound started playing.
    void OnEventStart() {
        ++m_active_events;
    }

    // An events-bus sound finished (reaped/stopped). Robust to spurious extra
    // calls: never underflows below zero.
    void OnEventEnd() {
        if (m_active_events > 0)
            --m_active_events;
    }

    // Hard reset (e.g. audio manager shutdown): no active events, no duck.
    void Reset() {
        m_active_events = 0;
        m_duck01 = 0.0f;
    }

    // Advance the envelope by wall-clock dt (seconds) and return duck01.
    // Linear ramps matching DuckGainAt: toward 1 at rate 1/attack while any
    // event is active, toward 0 at rate 1/release otherwise. Negative dt is
    // ignored (no-op); a huge dt simply saturates at the target.
    float Advance(float dt_seconds) {
        if (dt_seconds > 0.0f) {
            if (m_active_events > 0) {
                if (m_params.attack_seconds <= 0.0f) {
                    m_duck01 = 1.0f;
                } else {
                    m_duck01 = std::min(1.0f, m_duck01 + dt_seconds / m_params.attack_seconds);
                }
            } else {
                if (m_params.release_seconds <= 0.0f) {
                    m_duck01 = 0.0f;
                } else {
                    m_duck01 = std::max(0.0f, m_duck01 - dt_seconds / m_params.release_seconds);
                }
            }
        }
        return m_duck01;
    }

    float duck01() const {
        return m_duck01;
    }
    int active_events() const {
        return m_active_events;
    }
    bool IsIdle() const {
        return m_active_events == 0 && m_duck01 <= 0.0f;
    }

    // Per-bus gains derived from the shared envelope.
    float AmbientGain() const {
        return DuckGain(m_duck01, m_params.floor_gain);
    }
    float MusicGain() const {
        return DuckGain(m_duck01, m_params.music_floor_gain);
    }

private:
    DuckParams m_params{};
    int m_active_events = 0;
    float m_duck01 = 0.0f;
};

} // namespace Luminumbra::Client::Audio
