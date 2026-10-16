#pragma once

#if defined(LUMINUMBRA_ENABLE_STEAM) || defined(LUMINUMBRA_ENABLE_GNS)

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "LockstepSession.h"
#include "MessageReceiveQueue.h"
#include "steam/steamnetworkingtypes.h"

namespace Luminumbra::Net {

// The Steamworks SDK and standalone GameNetworkingSockets expose the same
// socket API. Only their library lifetime and callback pump differ.
struct NetSocketsBackend {
    bool (*initialize)(std::uint32_t argument);
    void (*shutdown)();
    void (*run_callbacks)();
    bool ready = false;
};

bool InitializeNetSocketsBackend(NetSocketsBackend& backend, std::uint32_t argument = 0);
void ShutdownNetSocketsBackend(NetSocketsBackend& backend);
void RunNetSocketsBackendCallbacks(NetSocketsBackend& backend);

class NetSocketsTransport : public ILockstepTransport {
public:
    NetSocketsTransport(NetSocketsBackend& backend, const char* name);
    ~NetSocketsTransport() override;

    NetSocketsTransport(const NetSocketsTransport&) = delete;
    NetSocketsTransport& operator=(const NetSocketsTransport&) = delete;

    bool Listen(std::uint16_t port);
    bool Connect(const std::string& host, std::uint16_t port);

    // Refusals: oversized for this backend (terminal, no bytes written), permanent
    // backend failure (terminal, closes after draining), or temporary capacity
    // (transient, no bytes accepted; retry the same bytes after draining receives).
    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override;
    [[nodiscard]] std::size_t MaxFramePayloadBytes() const override;
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override;
    bool TryReceiveFrameWithDelivery(std::vector<std::uint8_t>& out,
                                     FrameDelivery& delivery) override;
    void CloseWithReason(const std::string& reason) override;
    [[nodiscard]] bool IsPeerConnected() const override;
    void Close() override;

    static void OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);

private:
    void HandleConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void DrainInto();

    NetSocketsBackend& m_backend;
    const char* m_name;
    HSteamListenSocket m_listen = 0; // k_HSteamListenSocket_Invalid
    HSteamNetConnection m_conn = 0;  // k_HSteamNetConnection_Invalid
    bool m_connected = false;
    bool m_ever_connected = false; // only established connections may drain before closure
    bool m_closed = false;
    bool m_peer_closed = false; // drain the backend before releasing the connection
    bool m_is_host = false;
    MessageReceiveQueue m_recv;
    // One delivery flag per queued frame, bounded by kReceiveQueueFrameLimit (1024).
    std::deque<FrameDelivery> m_recv_delivery;
    std::string m_close_reason;
};

} // namespace Luminumbra::Net

#endif // defined(LUMINUMBRA_ENABLE_STEAM) || defined(LUMINUMBRA_ENABLE_GNS)
