#include "GnsTransport.h"

#ifdef LUMINUMBRA_ENABLE_GNS

#include "steam/isteamnetworkingsockets.h"
#include "steam/steamnetworkingsockets.h"

#include "../core/Log.h"

namespace Luminumbra::Net {

namespace {
bool InitializeGns(std::uint32_t) {
    SteamNetworkingErrMsg err{};
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        LUMINUMBRA_CORE_ERROR("GnsLink: GameNetworkingSockets_Init failed: {}", err);
        return false;
    }
    LUMINUMBRA_CORE_INFO("GnsLink: initialized (standalone GameNetworkingSockets, UDP).");
    return true;
}

void ShutdownGns() {
    GameNetworkingSockets_Kill();
}

void RunGnsCallbacks() {
    SteamNetworkingSockets()->RunCallbacks();
}

NetSocketsBackend& GnsBackend() {
    static NetSocketsBackend backend{&InitializeGns, &ShutdownGns, &RunGnsCallbacks};
    return backend;
}
} // namespace

bool GnsLink::Init() {
    return InitializeNetSocketsBackend(GnsBackend());
}

void GnsLink::Shutdown() {
    ShutdownNetSocketsBackend(GnsBackend());
}

bool GnsLink::initialized() {
    return GnsBackend().ready;
}

void GnsLink::RunCallbacks() {
    RunNetSocketsBackendCallbacks(GnsBackend());
}

GnsTransport::GnsTransport()
    : NetSocketsTransport(GnsBackend(), "GnsTransport") {}

GnsTransport::~GnsTransport() = default;

bool GnsTransport::Listen(std::uint16_t port) {
    return NetSocketsTransport::Listen(port);
}

bool GnsTransport::Connect(const std::string& host, std::uint16_t port) {
    return NetSocketsTransport::Connect(host, port);
}

bool GnsTransport::SendFrame(const std::vector<std::uint8_t>& frame, FrameDelivery delivery) {
    return NetSocketsTransport::SendFrame(frame, delivery);
}

bool GnsTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    return NetSocketsTransport::TryReceiveFrame(out);
}

bool GnsTransport::IsPeerConnected() const {
    return NetSocketsTransport::IsPeerConnected();
}

void GnsTransport::Close() {
    NetSocketsTransport::Close();
}

void GnsTransport::OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info) {
    NetSocketsTransport::OnConnStatusChangedStatic(info);
}

} // namespace Luminumbra::Net

#endif // LUMINUMBRA_ENABLE_GNS
