#pragma once

//  deterministic NETWORK-CONDITION simulator over the ILockstepTransport seam.
// The critique's identified unblocker: replication / client-prediction / reconciliation
// can't be validated over the wire on a single PC (memory single-pc-testing-constraint),
// so this provides an in-process paired transport that injects DETERMINISTIC latency,
// jitter (reorder), and packet loss. A test drives Tick and asserts behaviour under
// adverse conditions, reproducibly (run==replay), with no sockets / 2nd machine.
//
// Semantics mirror the FrameDelivery contract: RELIABLE frames are never dropped;
// UNRELIABLE frames may drop (most-recent-wins replication tolerates it). Loss/jitter are
// pure functions of a seed + the frame sequence number (splitmix64), so a session replays
// identically. Latency/jitter are measured in Ticks (engine ticks), never wall-clock.

#include "../core/DeterministicRng.h"
#include "LockstepSession.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace Luminumbra::Net {

struct NetworkConditions {
    int latency_ticks = 0; // base one-way delay (ticks)
    int jitter_ticks = 0;  // deterministic +/- delay jitter (reorders frames)
    int drop_one_in = 0;   // 0 = no loss; else drop ~1/N UNRELIABLE frames (by seq)
    std::uint64_t seed = 1;
};

// A paired in-process transport with simulated conditions. A and B are the two ends;
// Tick advances the shared sim clock (delivers frames whose deliver-tick has arrived).
class NetworkSim {
public:
    explicit NetworkSim(NetworkConditions cond)
        : m_cond(cond) {}

    void Tick() {
        ++m_tick;
    }
    [[nodiscard]] std::uint64_t now() const {
        return m_tick;
    }

    [[nodiscard]] ILockstepTransport& A() {
        return m_a;
    }
    [[nodiscard]] ILockstepTransport& B() {
        return m_b;
    }

private:
    struct PendingFrame {
        std::uint64_t deliver_tick = 0;
        std::uint64_t seq = 0;
        std::vector<std::uint8_t> data;
    };

    bool Send(bool isA, const std::vector<std::uint8_t>& f, FrameDelivery d) {
        const bool selfOpen = isA ? m_aOpen : m_bOpen;
        const bool peerOpen = isA ? m_bOpen : m_aOpen;
        if (!selfOpen || !peerOpen)
            return false;
        const std::uint64_t seq = m_seq++;
        // Loss: UNRELIABLE only, deterministic from the seq.
        if (d == FrameDelivery::Unreliable && m_cond.drop_one_in > 0) {
            const std::uint64_t r = luminumbra::core::DeterministicRng::mix(
                m_cond.seed ^ (seq * 0x9E3779B97F4A7C15ull + 0x1234567ull));
            if ((r % static_cast<std::uint64_t>(m_cond.drop_one_in)) == 0) {
                return true; // accepted by the sender, lost in transit (correct UDP semantics)
            }
        }
        std::int64_t delay = m_cond.latency_ticks;
        if (m_cond.jitter_ticks > 0) {
            const std::uint64_t r = luminumbra::core::DeterministicRng::mix(m_cond.seed * 3u + seq);
            delay += static_cast<std::int64_t>(
                         r % (2u * static_cast<unsigned>(m_cond.jitter_ticks) + 1u)) -
                     m_cond.jitter_ticks;
        }
        if (delay < 0)
            delay = 0;
        PendingFrame pf;
        pf.deliver_tick = m_tick + static_cast<std::uint64_t>(delay);
        pf.seq = seq;
        pf.data = f;
        (isA ? m_ab : m_ba).push_back(std::move(pf));
        return true;
    }

    bool Recv(bool isA, std::vector<std::uint8_t>& out) {
        std::deque<PendingFrame>& q = isA ? m_ba : m_ab; // A receives from the B->A queue
        int best = -1;
        for (int i = 0; i < static_cast<int>(q.size()); ++i) {
            if (q[i].deliver_tick > m_tick)
                continue;
            if (best < 0 || q[i].deliver_tick < q[best].deliver_tick ||
                (q[i].deliver_tick == q[best].deliver_tick && q[i].seq < q[best].seq)) {
                best = i;
            }
        }
        if (best < 0)
            return false;
        out = std::move(q[best].data);
        q.erase(q.begin() + best);
        return true;
    }

    [[nodiscard]] bool PeerOpen(bool isA) const {
        return isA ? m_bOpen : m_aOpen;
    }
    void CloseEnd(bool isA) {
        (isA ? m_aOpen : m_bOpen) = false;
    }

    class Ep final : public ILockstepTransport {
    public:
        Ep(NetworkSim* sim, bool isA)
            : m_sim(sim)
            , m_isA(isA) {}
        bool SendFrame(const std::vector<std::uint8_t>& f,
                       FrameDelivery d = FrameDelivery::Reliable) override {
            return m_sim->Send(m_isA, f, d);
        }
        bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
            return m_sim->Recv(m_isA, out);
        }
        [[nodiscard]] bool IsPeerConnected() const override {
            return m_sim->PeerOpen(m_isA);
        }
        void Close() override {
            m_sim->CloseEnd(m_isA);
        }

    private:
        NetworkSim* m_sim;
        bool m_isA;
    };

    NetworkConditions m_cond;
    std::uint64_t m_tick = 0;
    std::uint64_t m_seq = 0;
    std::deque<PendingFrame> m_ab; // A -> B in flight
    std::deque<PendingFrame> m_ba; // B -> A in flight
    bool m_aOpen = true;
    bool m_bOpen = true;
    Ep m_a{this, true};
    Ep m_b{this, false};
};

} // namespace Luminumbra::Net
