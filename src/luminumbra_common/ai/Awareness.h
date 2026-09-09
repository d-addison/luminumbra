#pragma once

// perception FUSION — the awareness / detection-meter state machine that
// turns a fused sensory signal (vision + hearing + smell) into a graduated
// awareness state plus a remembered last-known position. Detection is not
// binary: a meter accumulates while a target is sensed and
// decays when it is not, so a creature grows suspicious, goes alert, keeps
// SEARCHING the last-known spot after losing the target, and only forgets once the
// meter drains. This is what makes stalking / stealth / pursuit believable, and is
// where the three senses combine before feeding the InstinctPlanner.
//
// PURE + DETERMINISTIC: a single update is a pure function of its inputs (no RNG,
// no wall-clock, float-only). RunPerceptionSystemOnTick uses this update for
// entities with PerceptionComponent + AwarenessComponent in GameSession's fixed tick.

namespace luminumbra::ai {

enum class AwarenessState {
    Unaware,    // meter below suspicion
    Suspicious, // something faint registered
    Alert,      // high awareness, currently sensing
    Searching,  // was alert, lost the signal -> heads to last-known
    Engaged,    // strong current signal at/above the engage threshold
};

struct AwarenessParams {
    float gain = 2.0f;           // meter rise per (signal * second)
    float decay = 0.5f;          // meter fall per second with no signal
    float suspicious_at = 0.15f; // meter thresholds in [0,1]
    float alert_at = 0.5f;
    float engaged_at = 0.85f;
};

struct Awareness {
    float meter = 0.0f; // [0,1] accumulated detection
    AwarenessState state = AwarenessState::Unaware;
    bool has_last_known = false; // a target position is remembered
    float last_known_x = 0.0f;
    float last_known_z = 0.0f;
    float time_since_signal = 0.0f; // seconds since a non-zero signal
};

// Advance awareness one step. `signal` is the fused sense strength this tick
// (>= 0; 0 = nothing detected); when signal > 0, (tx,tz) is the sensed position.
// `dt` is the fixed tick delta (seconds). Pure — returns the next Awareness.
[[nodiscard]] inline Awareness UpdateAwareness(
    const Awareness& prev, float signal, float tx, float tz, float dt, const AwarenessParams& p) {
    Awareness a = prev;
    const bool sensing = signal > 0.0f;
    if (sensing) {
        a.meter += p.gain * signal * dt;
        a.has_last_known = true;
        a.last_known_x = tx;
        a.last_known_z = tz;
        a.time_since_signal = 0.0f;
    } else {
        a.meter -= p.decay * dt;
        a.time_since_signal += dt;
    }
    if (a.meter < 0.0f)
        a.meter = 0.0f;
    if (a.meter > 1.0f)
        a.meter = 1.0f;

    // Derive the state from the meter + whether we are currently sensing. Losing
    // the signal while still highly aware = Searching (go to last-known); as the
    // meter decays it falls back through Suspicious to Unaware (which clears the
    // stale memory so the creature gives up).
    if (sensing && a.meter >= p.engaged_at) {
        a.state = AwarenessState::Engaged;
    } else if (a.meter >= p.alert_at) {
        a.state = sensing ? AwarenessState::Alert : AwarenessState::Searching;
    } else if (a.meter >= p.suspicious_at) {
        a.state = AwarenessState::Suspicious;
    } else {
        a.state = AwarenessState::Unaware;
        a.has_last_known = false; // forgotten
    }
    return a;
}

} // namespace luminumbra::ai
