#include "ModeHelpers.h"
#include "core/Profiler.h"

namespace {

bool PollRuntimeTcpHello(RuntimeTcpClientSlot& slot,
                         const Luminumbra::Net::HelloMsg& expected,
                         std::chrono::steady_clock::time_point now) {
    if (now >= slot.hello_deadline) {
        LUMINUMBRA_CORE_ERROR("net-host: Hello handshake deadline expired for client slot {}",
                              slot.client_id);
        slot.transport->Close();
        slot.transport.reset();
        slot.awaiting_hello = false;
        return false;
    }

    std::vector<std::uint8_t> frame;
    if (!slot.transport->TryReceiveFrame(frame)) {
        if (!slot.transport->IsPeerConnected()) {
            slot.transport.reset();
            slot.awaiting_hello = false;
        }
        return false;
    }
    Luminumbra::Net::HelloMsg hello;
    slot.awaiting_hello = false;
    if (!Luminumbra::Net::DecodeHello(frame, hello) ||
        !Luminumbra::Net::HelloMatches(expected, hello) || hello.client_id != slot.client_id) {
        LUMINUMBRA_CORE_ERROR("net-host: incompatible session identity (including resolved "
                              "preset content hash) for client {}",
                              slot.client_id);
        slot.transport->Close();
        slot.transport.reset();
        return false;
    }
    slot.accepted = true;
    slot.connected = true;
    return true;
}

} // namespace

void StartRuntimeTcpAccept(RuntimeTcpClientSlot& slot) {
    slot.transport = std::make_unique<Luminumbra::Net::TcpTransport>();
    Luminumbra::Net::TcpTransport* transport = slot.transport.get();
    const std::uint16_t port = slot.port;
    slot.accepting = true;
    slot.accept_result = std::async(std::launch::async, [transport, port]() {
        LUMIN_PROFILE_THREAD("TCP accept worker");
        LUMIN_PROFILE_ZONE_N("TCP accept worker job");
        return transport->Listen(port, /*timeout_ms=*/250);
    });
}

bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot, const Luminumbra::Net::HelloMsg& expected) {
    return PollRuntimeTcpAccept(slot, expected, std::chrono::steady_clock::now());
}

bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot,
                          const Luminumbra::Net::HelloMsg& expected,
                          std::chrono::steady_clock::time_point now) {
    if (slot.accepted) {
        return false;
    }
    if (slot.awaiting_hello) {
        return PollRuntimeTcpHello(slot, expected, now);
    }
    if (!slot.accepting) {
        StartRuntimeTcpAccept(slot);
    }
    if (!slot.accept_result.valid() ||
        LUMIN_PROFILE_WAIT("TCP accept future poll",
                           slot.accept_result.wait_for(std::chrono::milliseconds(0))) !=
            std::future_status::ready) {
        return false;
    }

    const bool accepted = LUMIN_PROFILE_WAIT("TCP accept future get", slot.accept_result.get());
    slot.accepting = false;
    if (accepted) {
        slot.awaiting_hello = true;
        slot.hello_deadline = now + RuntimeTcpClientSlot::kHelloTimeout;
        return false;
    }

    slot.transport.reset();
    return false;
}

void CloseRuntimeTcpSlot(RuntimeTcpClientSlot& slot) {
    if (slot.accepting && slot.accept_result.valid()) {
        LUMIN_PROFILE_WAIT("TCP accept future wait", slot.accept_result.wait());
    }
    if (slot.transport) {
        slot.transport->Close();
    }
}
