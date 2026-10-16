#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "LockstepSession.h"

namespace Luminumbra::Net {

// Local 30 Hz handshake time, independent of simulation progress: 150 ticks is
// nominally five seconds, separate from the runtime TCP steady-clock deadline.
inline constexpr std::uint64_t kAdmissionTimeoutTicks = 150;
inline constexpr std::size_t kAdmissionDrainLimit = 32;

enum class ReplicationAdmissionRole {
    Host,
    Joiner
};
enum class ReplicationAdmissionPhase {
    Idle,
    AwaitHello,
    SendHello,
    SendingAck,
    AwaitAck,
    Admitted,
    Failed
};
enum class ReplicationAdmissionAction {
    Discard,
    SendHello,
    SendAck,
    Forward
};

// Pure policy: callers supply monotonic local handshake ticks and perform actions.
class ReplicationAdmissionState {
public:
    ReplicationAdmissionState(ReplicationAdmissionRole role,
                              HelloMsg local,
                              std::uint32_t peer_client_id);
    ReplicationAdmissionAction Begin(std::uint64_t now);
    ReplicationAdmissionAction OnFrame(const std::vector<std::uint8_t>& frame,
                                       FrameDelivery delivery);
    void OnHelloSendResult(bool sent, std::uint64_t now);
    void OnAckSendResult(bool sent);
    void CheckDeadline(std::uint64_t now);
    void Fail(const std::string& reason);

    [[nodiscard]] bool admitted() const {
        return m_phase == ReplicationAdmissionPhase::Admitted;
    }
    [[nodiscard]] bool failed() const {
        return m_phase == ReplicationAdmissionPhase::Failed;
    }
    [[nodiscard]] ReplicationAdmissionPhase phase() const {
        return m_phase;
    }
    [[nodiscard]] const std::string& reason() const {
        return m_reason;
    }
    [[nodiscard]] std::vector<std::uint8_t> hello_frame() const {
        return EncodeHello(m_local);
    }

private:
    ReplicationAdmissionRole m_role;
    HelloMsg m_local;
    std::uint32_t m_peer_client_id;
    ReplicationAdmissionPhase m_phase = ReplicationAdmissionPhase::Idle;
    std::uint64_t m_started = 0;
    std::string m_reason;
};

// Non-owning: the underlying connection must outlive this adapter, and this
// adapter must outlive endpoints registered with it. Control never reaches replication.
class AdmittedReplicationTransport final : public ILockstepTransport {
public:
    AdmittedReplicationTransport(ILockstepTransport& transport,
                                 ReplicationAdmissionRole role,
                                 HelloMsg local,
                                 std::uint32_t peer_client_id);
    void Begin(std::uint64_t now);
    void PumpAdmission(std::uint64_t now);
    [[nodiscard]] const ReplicationAdmissionState& admission() const {
        return m_state;
    }

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override;
    [[nodiscard]] std::size_t MaxFramePayloadBytes() const override {
        return m_transport.MaxFramePayloadBytes();
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override;
    bool TryReceiveFrameWithDelivery(std::vector<std::uint8_t>& out,
                                     FrameDelivery& delivery) override;
    [[nodiscard]] bool IsPeerConnected() const override;
    void Close() override;
    void CloseWithReason(const std::string& reason) override;

private:
    void Apply(ReplicationAdmissionAction action);
    void CloseIfFailed();
    ILockstepTransport& m_transport;
    ReplicationAdmissionState m_state;
    std::uint64_t m_now = 0;
    bool m_failure_closed = false;
};

} // namespace Luminumbra::Net
