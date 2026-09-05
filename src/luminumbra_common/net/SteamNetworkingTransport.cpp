#include "SteamNetworkingTransport.h"

#ifdef LUMINUMBRA_ENABLE_STEAM

#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/steam_api.h"

#include "../core/Log.h"

namespace Luminumbra::Net {

namespace {
bool InitializeSteam(std::uint32_t dev_app_id) {
    // Developers without a published app id may explicitly provide steam_appid.txt
    // (for example, with Spacewar id 480). Never create it here: shipping it or
    // writing it automatically would bypass Steam's ownership check.
    SteamErrMsg err{};
    if (SteamAPI_InitEx(&err) != k_ESteamAPIInitResult_OK) {
        LUMINUMBRA_CORE_ERROR(
            "SteamLink: SteamAPI_Init failed: {} (is the Steam client running, app id {}?)",
            err,
            dev_app_id);
        return false;
    }
    if (SteamNetworkingUtils() == nullptr || SteamNetworkingSockets() == nullptr) {
        LUMINUMBRA_CORE_ERROR("SteamLink: networking interfaces unavailable after init");
        SteamAPI_Shutdown();
        return false;
    }
    // Bring up Steam's networking subsystem (also a prerequisite for P2P/SDR later).
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    LUMINUMBRA_CORE_INFO("SteamLink: initialized (Steamworks SDK, app id {}).", dev_app_id);
    return true;
}

void ShutdownSteam() {
    SteamAPI_Shutdown();
}

void RunSteamCallbacks() {
    SteamAPI_RunCallbacks();
}

NetSocketsBackend& SteamBackend() {
    static NetSocketsBackend backend{&InitializeSteam, &ShutdownSteam, &RunSteamCallbacks};
    return backend;
}
} // namespace

bool SteamLink::Init(std::uint32_t dev_app_id) {
    return InitializeNetSocketsBackend(SteamBackend(), dev_app_id);
}

void SteamLink::Shutdown() {
    ShutdownNetSocketsBackend(SteamBackend());
}

bool SteamLink::initialized() {
    return SteamBackend().ready;
}

void SteamLink::RunCallbacks() {
    RunNetSocketsBackendCallbacks(SteamBackend());
}

SteamNetworkingTransport::SteamNetworkingTransport()
    : NetSocketsTransport(SteamBackend(), "SteamNetworkingTransport") {}

SteamNetworkingTransport::~SteamNetworkingTransport() = default;

bool SteamNetworkingTransport::Listen(std::uint16_t port) {
    return NetSocketsTransport::Listen(port);
}

bool SteamNetworkingTransport::Connect(const std::string& host, std::uint16_t port) {
    return NetSocketsTransport::Connect(host, port);
}

bool SteamNetworkingTransport::SendFrame(const std::vector<std::uint8_t>& frame,
                                         FrameDelivery delivery) {
    return NetSocketsTransport::SendFrame(frame, delivery);
}

bool SteamNetworkingTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    return NetSocketsTransport::TryReceiveFrame(out);
}

bool SteamNetworkingTransport::IsPeerConnected() const {
    return NetSocketsTransport::IsPeerConnected();
}

void SteamNetworkingTransport::Close() {
    NetSocketsTransport::Close();
}

void SteamNetworkingTransport::OnConnStatusChangedStatic(
    SteamNetConnectionStatusChangedCallback_t* info) {
    NetSocketsTransport::OnConnStatusChangedStatic(info);
}

} // namespace Luminumbra::Net

#endif // LUMINUMBRA_ENABLE_STEAM
