#include "ReplicationAdmission.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "ReplicationProtocol.h"

namespace Luminumbra::Net {

namespace {
bool DecodeAdmissionHello(const std::vector<std::uint8_t>& frame, HelloMsg& hello) {
    return DecodeHello(frame, hello) && EncodeHello(hello) == frame;
}

bool PayloadLength(const std::vector<std::uint8_t>& frame, std::uint32_t& length) {
    if (frame.size() < 5)
        return false;
    length = 0;
    for (unsigned i = 0; i < 4; ++i)
        length |= static_cast<std::uint32_t>(frame[1 + i]) << (8 * i);
    return frame.size() - 5 == length;
}
} // namespace

ReplicationAdmissionState::ReplicationAdmissionState(ReplicationAdmissionRole role,
                                                     HelloMsg local,
                                                     std::uint32_t peer_client_id)
    : m_role(role)
    , m_local(std::move(local))
    , m_peer_client_id(peer_client_id) {
    if (m_role == ReplicationAdmissionRole::Host)
        m_local.client_id = 0;
}

ReplicationAdmissionAction ReplicationAdmissionState::Begin(std::uint64_t now) {
    if (m_phase != ReplicationAdmissionPhase::Idle)
        return ReplicationAdmissionAction::Discard;
    m_started = now;
    m_phase = m_role == ReplicationAdmissionRole::Host ? ReplicationAdmissionPhase::AwaitHello
                                                       : ReplicationAdmissionPhase::SendHello;
    return m_role == ReplicationAdmissionRole::Host ? ReplicationAdmissionAction::Discard
                                                    : ReplicationAdmissionAction::SendHello;
}

void ReplicationAdmissionState::Fail(const std::string& reason) {
    if (failed())
        return;
    m_reason = reason;
    m_phase = ReplicationAdmissionPhase::Failed;
}

void ReplicationAdmissionState::OnHelloSendResult(bool sent, std::uint64_t now) {
    if (m_phase != ReplicationAdmissionPhase::SendHello)
        return;
    if (!sent) {
        Fail("admission Hello send failed");
        return;
    }
    m_started = now;
    m_phase = ReplicationAdmissionPhase::AwaitAck;
}

void ReplicationAdmissionState::OnAckSendResult(bool sent) {
    if (m_phase != ReplicationAdmissionPhase::SendingAck && !admitted())
        return;
    if (!sent)
        Fail("admission ack send failed");
    else
        m_phase = ReplicationAdmissionPhase::Admitted;
}

void ReplicationAdmissionState::CheckDeadline(std::uint64_t now) {
    if ((m_phase == ReplicationAdmissionPhase::AwaitHello ||
         m_phase == ReplicationAdmissionPhase::AwaitAck ||
         m_phase == ReplicationAdmissionPhase::SendingAck) &&
        now >= m_started && now - m_started >= kAdmissionTimeoutTicks) {
        Fail(m_role == ReplicationAdmissionRole::Host ? "admission Hello timeout"
                                                      : "admission ack timeout");
    }
}

ReplicationAdmissionAction
ReplicationAdmissionState::OnFrame(const std::vector<std::uint8_t>& frame, FrameDelivery delivery) {
    if (failed() || m_phase == ReplicationAdmissionPhase::Idle ||
        m_phase == ReplicationAdmissionPhase::SendHello)
        return ReplicationAdmissionAction::Discard;
    std::uint32_t length = 0;
    if (!PayloadLength(frame, length)) {
        Fail("malformed admission frame length");
        return ReplicationAdmissionAction::Discard;
    }
    if (frame[0] != static_cast<std::uint8_t>(MessageType::Hello))
        return admitted() ? ReplicationAdmissionAction::Forward
                          : ReplicationAdmissionAction::Discard;

    // Length wins over magic: a command tick may itself begin with LSTP1.
    if (length == kUsercmdPayloadBytes) {
        UsercmdMsg command;
        if (!DecodeUsercmd(frame, command)) {
            Fail("malformed user command");
            return ReplicationAdmissionAction::Discard;
        }
        return admitted() ? ReplicationAdmissionAction::Forward
                          : ReplicationAdmissionAction::Discard;
    }
    if (delivery != FrameDelivery::Reliable) {
        Fail("unreliable admission Hello");
        return ReplicationAdmissionAction::Discard;
    }
    if (length < sizeof(kLockstepMagic) ||
        !std::equal(std::begin(kLockstepMagic), std::end(kLockstepMagic), frame.begin() + 5)) {
        Fail("malformed admission Hello");
        return ReplicationAdmissionAction::Discard;
    }
    // DecodeHello intentionally retains its permissive TCP/lockstep behavior.
    // Inspect version first because that decoder rejects older versions itself.
    if (length >= 7 &&
        (frame[10] != static_cast<std::uint8_t>(kLockstepProtocolVersion) || frame[11] != 0)) {
        Fail("protocol version mismatch");
        return ReplicationAdmissionAction::Discard;
    }
    HelloMsg peer;
    if (!DecodeAdmissionHello(frame, peer)) {
        Fail("malformed admission Hello");
        return ReplicationAdmissionAction::Discard;
    }
    if (peer.client_id != m_peer_client_id) {
        Fail("admission client id mismatch");
    } else if (m_local.protocol_version != kLockstepProtocolVersion ||
               peer.protocol_version != m_local.protocol_version) {
        Fail("protocol version mismatch");
    } else if (m_local.system_config_identity.empty() || peer.system_config_identity.empty() ||
               peer.system_config_identity != m_local.system_config_identity) {
        Fail("SystemConfig identity mismatch");
    } else if (m_local.preset_content_hash.empty() || peer.preset_content_hash.empty() ||
               peer.preset_content_hash != m_local.preset_content_hash) {
        Fail("preset content hash mismatch");
    } else if (!HelloMatches(m_local, peer)) {
        Fail("admission seed, preset or tick rate mismatch");
    }
    if (failed())
        return ReplicationAdmissionAction::Discard;
    if (m_role == ReplicationAdmissionRole::Host) {
        if (!admitted())
            m_phase = ReplicationAdmissionPhase::SendingAck;
        return ReplicationAdmissionAction::SendAck;
    }
    m_phase = ReplicationAdmissionPhase::Admitted;
    return ReplicationAdmissionAction::Discard;
}

AdmittedReplicationTransport::AdmittedReplicationTransport(ILockstepTransport& transport,
                                                           ReplicationAdmissionRole role,
                                                           HelloMsg local,
                                                           std::uint32_t peer_client_id)
    : m_transport(transport)
    , m_state(role, std::move(local), peer_client_id) {}

void AdmittedReplicationTransport::CloseIfFailed() {
    if (m_state.failed() && !m_failure_closed) {
        m_failure_closed = true;
        m_transport.CloseWithReason(m_state.reason());
    }
}

void AdmittedReplicationTransport::Apply(ReplicationAdmissionAction action) {
    if (action == ReplicationAdmissionAction::SendHello) {
        m_state.OnHelloSendResult(
            m_transport.SendFrame(m_state.hello_frame(), FrameDelivery::Reliable), m_now);
    } else if (action == ReplicationAdmissionAction::SendAck) {
        m_state.OnAckSendResult(
            m_transport.SendFrame(m_state.hello_frame(), FrameDelivery::Reliable));
    }
    CloseIfFailed();
}

void AdmittedReplicationTransport::Begin(std::uint64_t now) {
    m_now = now;
    if (!m_transport.IsPeerConnected())
        m_state.Fail("admission disconnected");
    Apply(m_state.Begin(now));
}

void AdmittedReplicationTransport::PumpAdmission(std::uint64_t now) {
    m_now = now;
    if (!m_transport.IsPeerConnected())
        m_state.Fail("admission disconnected");
    m_state.CheckDeadline(now);
    CloseIfFailed();
    if (m_state.admitted() || m_state.failed())
        return;
    for (std::size_t i = 0; i < kAdmissionDrainLimit; ++i) {
        std::vector<std::uint8_t> frame;
        FrameDelivery delivery = FrameDelivery::Reliable;
        if (!m_transport.TryReceiveFrameWithDelivery(frame, delivery))
            break;
        Apply(m_state.OnFrame(frame, delivery));
        // Leave subsequent frames in the transport for the replication endpoint.
        if (m_state.admitted() || m_state.failed())
            break;
    }
    m_state.CheckDeadline(now);
    CloseIfFailed();
}

bool AdmittedReplicationTransport::SendFrame(const std::vector<std::uint8_t>& frame,
                                             FrameDelivery delivery) {
    if (!m_transport.IsPeerConnected()) {
        CloseWithReason("admission disconnected");
        return false;
    }
    if (!m_state.admitted())
        return false;
    // Only admission owns Hello sends, including retransmitted acknowledgements.
    if (!frame.empty() && frame[0] == static_cast<std::uint8_t>(MessageType::Hello) &&
        frame.size() != 5 + kUsercmdPayloadBytes)
        return false;
    return m_transport.SendFrame(frame, delivery);
}

bool AdmittedReplicationTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    FrameDelivery delivery = FrameDelivery::Reliable;
    return TryReceiveFrameWithDelivery(out, delivery);
}

bool AdmittedReplicationTransport::TryReceiveFrameWithDelivery(std::vector<std::uint8_t>& out,
                                                               FrameDelivery& delivery) {
    if (!m_transport.IsPeerConnected()) {
        CloseWithReason("admission disconnected");
        return false;
    }
    if (!m_state.admitted()) {
        PumpAdmission(m_now);
        return false;
    }
    for (std::size_t i = 0; i < kAdmissionDrainLimit; ++i) {
        if (!m_transport.TryReceiveFrameWithDelivery(out, delivery))
            return false;
        const auto action = m_state.OnFrame(out, delivery);
        Apply(action);
        if (m_state.failed())
            return false;
        if (action == ReplicationAdmissionAction::Forward)
            return true;
    }
    return false;
}

bool AdmittedReplicationTransport::IsPeerConnected() const {
    return !m_state.failed() && m_transport.IsPeerConnected();
}

void AdmittedReplicationTransport::Close() {
    CloseWithReason("admission connection closed");
}

void AdmittedReplicationTransport::CloseWithReason(const std::string& reason) {
    m_state.Fail(reason);
    CloseIfFailed();
}

} // namespace Luminumbra::Net
