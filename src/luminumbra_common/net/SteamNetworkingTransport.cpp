#include "SteamNetworkingTransport.h"

#ifdef LUMINUMBRA_ENABLE_STEAM

#include <algorithm>
#include <cstdio>
#include <vector>

#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/steam_api.h"

#include "../core/Log.h"

namespace Luminumbra::Net {

namespace {
// v1: single peer per transport. The global connection-status callback is
// process-wide, so it fans out to every live transport; each one claims the
// events for its own listen socket / connection.
std::vector<SteamNetworkingTransport*>& ActiveTransports() {
    static std::vector<SteamNetworkingTransport*> s;
    return s;
}
bool g_steam_ready = false;
} // namespace

bool SteamLink::Init(std::uint32_t dev_app_id) {
    if (g_steam_ready)
        return true;
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
    // Route connection-status changes (accept/connect/close) to our static dispatcher.
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &SteamNetworkingTransport::OnConnStatusChangedStatic);
    // Bring up Steam's networking subsystem (also a prerequisite for P2P/SDR later).
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    g_steam_ready = true;
    LUMINUMBRA_CORE_INFO("SteamLink: initialized (Steamworks SDK, app id {}).", dev_app_id);
    return true;
}

void SteamLink::Shutdown() {
    if (!g_steam_ready)
        return;
    SteamAPI_Shutdown();
    g_steam_ready = false;
}

bool SteamLink::initialized() {
    return g_steam_ready;
}

void SteamLink::RunCallbacks() {
    if (g_steam_ready)
        SteamAPI_RunCallbacks();
}

SteamNetworkingTransport::SteamNetworkingTransport() {
    ActiveTransports().push_back(this);
}

SteamNetworkingTransport::~SteamNetworkingTransport() {
    Close();
    auto& v = ActiveTransports();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

bool SteamNetworkingTransport::Listen(std::uint16_t port) {
    if (!g_steam_ready)
        return false;
    m_is_host = true;
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port; // any interface, given port
    m_listen = SteamNetworkingSockets()->CreateListenSocketIP(addr, 0, nullptr);
    return m_listen != k_HSteamListenSocket_Invalid;
}

bool SteamNetworkingTransport::Connect(const std::string& host, std::uint16_t port) {
    if (!g_steam_ready)
        return false;
    m_is_host = false;
    SteamNetworkingIPAddr addr;
    addr.Clear();
    char hostport[128];
    std::snprintf(hostport, sizeof(hostport), "%s:%u", host.c_str(), static_cast<unsigned>(port));
    if (!addr.ParseString(hostport)) {
        LUMINUMBRA_CORE_ERROR("SteamNetworkingTransport: bad address '{}'", hostport);
        return false;
    }
    m_conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    return m_conn != k_HSteamNetConnection_Invalid;
}

void SteamNetworkingTransport::OnConnStatusChangedStatic(
    SteamNetConnectionStatusChangedCallback_t* info) {
    for (SteamNetworkingTransport* t : ActiveTransports()) {
        t->HandleConnStatusChanged(info);
    }
}

void SteamNetworkingTransport::HandleConnStatusChanged(
    SteamNetConnectionStatusChangedCallback_t* info) {
    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Host: a client is dialling our listen socket -> accept it as our peer.
            if (m_is_host && m_listen != k_HSteamListenSocket_Invalid &&
                info->m_info.m_hListenSocket == m_listen &&
                m_conn == k_HSteamNetConnection_Invalid) {
                m_conn = info->m_hConn;
                SteamNetworkingSockets()->AcceptConnection(m_conn);
            }
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            if (info->m_hConn == m_conn)
                m_connected = true;
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            if (info->m_hConn == m_conn) {
                m_closed = true;
                m_connected = false;
                SteamNetworkingSockets()->CloseConnection(m_conn, 0, nullptr, false);
                m_conn = k_HSteamNetConnection_Invalid;
            }
            break;
        default:
            break;
    }
}

bool SteamNetworkingTransport::SendFrame(const std::vector<std::uint8_t>& frame,
                                         FrameDelivery delivery) {
    if (!g_steam_ready || m_conn == k_HSteamNetConnection_Invalid)
        return false;
    const int flags = (delivery == FrameDelivery::Reliable) ? k_nSteamNetworkingSend_Reliable
                                                            : k_nSteamNetworkingSend_Unreliable;
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, frame.data(), static_cast<std::uint32_t>(frame.size()), flags, nullptr);
    return r == k_EResultOK;
}

void SteamNetworkingTransport::DrainInto() {
    if (!g_steam_ready || m_conn == k_HSteamNetConnection_Invalid)
        return;
    SteamNetworkingMessage_t* msgs[32];
    const int n = SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_conn, msgs, 32);
    for (int i = 0; i < n; ++i) {
        SteamNetworkingMessage_t* m = msgs[i];
        const auto* p = static_cast<const std::uint8_t*>(m->m_pData);
        m_recv.emplace_back(p, p + m->m_cbSize);
        m->Release();
    }
}

bool SteamNetworkingTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    DrainInto();
    if (m_recv.empty())
        return false;
    out = std::move(m_recv.front());
    m_recv.pop_front();
    return true;
}

bool SteamNetworkingTransport::IsPeerConnected() const {
    return m_connected && !m_closed;
}

void SteamNetworkingTransport::Close() {
    if (m_conn != k_HSteamNetConnection_Invalid && g_steam_ready) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "closing", true);
    }
    m_conn = k_HSteamNetConnection_Invalid;
    if (m_listen != k_HSteamListenSocket_Invalid && g_steam_ready) {
        SteamNetworkingSockets()->CloseListenSocket(m_listen);
    }
    m_listen = k_HSteamListenSocket_Invalid;
    m_connected = false;
}

} // namespace Luminumbra::Net

#endif // LUMINUMBRA_ENABLE_STEAM
