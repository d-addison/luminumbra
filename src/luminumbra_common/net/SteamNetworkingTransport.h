#pragma once

// Steamworks transport for the authoritative-server replication stack.
// Implements the engine's ILockstepTransport seam over ISteamNetworkingSockets
// (the Steam SDK's build of GameNetworkingSockets): real UDP with reliable +
// unreliable channels, encryption, and -- via P2P/SDR later -- NAT traversal and
// Valve's relay network. The replication endpoints (ReplicationServer/Client) run
// over this unchanged, exactly as they do over LoopbackTransport / TcpTransport.
//
// v1 scope mirrors TcpTransport: direct IP connect, ONE peer per transport
// (Listen accepts a single connection). Lobbies / P2P / SDR are layered on top
// later. Whole-file is compiled out unless LUMINUMBRA_ENABLE_STEAM is defined
// (the SDK is owner-provided + non-redistributable), so the default build never
// depends on the Steam SDK.

#include "LockstepSession.h" // ILockstepTransport, FrameDelivery

#ifdef LUMINUMBRA_ENABLE_STEAM

#include <cstdint>
#include <string>
#include <vector>

#include "NetSocketsTransport.h"

namespace Luminumbra::Net {

// Initializes/owns the Steam API for networking. Call SteamLink::Init once before
// creating any SteamNetworkingTransport, and SteamLink::Shutdown at exit. Writes
// steam_appid.txt for the dev App ID (480 = Spacewar) if absent so SteamAPI_Init
// can attach to the running Steam client without a published app.
class SteamLink {
public:
    static bool Init(std::uint32_t dev_app_id = 480);
    static void Shutdown();
    static bool initialized();
    // Pumps Steam callbacks (connection-status changes -> accept/connect/close).
    // Call once per loop iteration on both host and client.
    static void RunCallbacks();
};

class SteamNetworkingTransport final : public NetSocketsTransport {
public:
    SteamNetworkingTransport();
    ~SteamNetworkingTransport() override;

    SteamNetworkingTransport(const SteamNetworkingTransport&) = delete;
    SteamNetworkingTransport& operator=(const SteamNetworkingTransport&) = delete;

    // Host: listen on `port` (all interfaces). Accept happens asynchronously via
    // the connection-status callback; poll IsPeerConnected after RunCallbacks.
    bool Listen(std::uint16_t port);
    // Client: connect to host:port. Connection completes asynchronously; poll
    // IsPeerConnected after RunCallbacks.
    bool Connect(const std::string& host, std::uint16_t port);

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override;
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override;
    [[nodiscard]] bool IsPeerConnected() const override;
    void Close() override;

    // Process-wide connection-status dispatcher (registered by SteamLink::Init).
    // Public so SteamLink can install it; fans out to every live transport.
    static void OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);
};

} // namespace Luminumbra::Net

#endif // LUMINUMBRA_ENABLE_STEAM
