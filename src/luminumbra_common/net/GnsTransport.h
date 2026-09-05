#pragma once

// standalone GameNetworkingSockets transport for the replication stack.
// Implements ILockstepTransport over Valve's open-source GameNetworkingSockets
// (UDP + reliable/unreliable + encryption), INDEPENDENT of the Steam client -- so
// two processes can connect on ONE machine, which the Steam SDK can't do
// (one-instance-per-app). This is the locally-testable real-UDP path for dev.
//
// Same ISteamNetworkingSockets API as SteamNetworkingTransport; only the init
// (GameNetworkingSockets_Init) and callback pump (SteamNetworkingSockets->
// RunCallbacks) differ. Whole file compiled out unless LUMINUMBRA_ENABLE_GNS.

#include "LockstepSession.h" // ILockstepTransport, FrameDelivery

#ifdef LUMINUMBRA_ENABLE_GNS

#include <cstdint>
#include <string>
#include <vector>

#include "NetSocketsTransport.h"

namespace Luminumbra::Net {

// Owns the standalone GNS library lifetime. Init once before creating transports;
// RunCallbacks once per loop iteration; Shutdown at exit.
class GnsLink {
public:
    static bool Init();
    static void Shutdown();
    static bool initialized();
    static void RunCallbacks();
};

class GnsTransport final : public NetSocketsTransport {
public:
    GnsTransport();
    ~GnsTransport() override;

    GnsTransport(const GnsTransport&) = delete;
    GnsTransport& operator=(const GnsTransport&) = delete;

    bool Listen(std::uint16_t port);                           // host
    bool Connect(const std::string& host, std::uint16_t port); // client

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override;
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override;
    [[nodiscard]] bool IsPeerConnected() const override;
    void Close() override;

    static void OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);
};

} // namespace Luminumbra::Net

#endif // LUMINUMBRA_ENABLE_GNS
