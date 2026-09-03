#include "GnsTransport.h"

#ifdef LUMINUMBRA_ENABLE_GNS

#include <algorithm>
#include <cstdio>
#include <vector>

#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/steamnetworkingsockets.h"

#include "../core/Log.h"

namespace Luminumbra::Net {

namespace {
std::vector<GnsTransport*>& ActiveTransports() {
    static std::vector<GnsTransport*> s;
    return s;
}
bool g_gns_ready = false;
} // namespace

bool GnsLink::Init() {
    if (g_gns_ready)
        return true;
    SteamNetworkingErrMsg err{};
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        LUMINUMBRA_CORE_ERROR("GnsLink: GameNetworkingSockets_Init failed: {}", err);
        return false;
    }
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &GnsTransport::OnConnStatusChangedStatic);
    g_gns_ready = true;
    LUMINUMBRA_CORE_INFO("GnsLink: initialized (standalone GameNetworkingSockets, UDP).");
    return true;
}

void GnsLink::Shutdown() {
    if (!g_gns_ready)
        return;
    GameNetworkingSockets_Kill();
    g_gns_ready = false;
}

bool GnsLink::initialized() {
    return g_gns_ready;
}

void GnsLink::RunCallbacks() {
    if (g_gns_ready)
        SteamNetworkingSockets()->RunCallbacks();
}

GnsTransport::GnsTransport() {
    ActiveTransports().push_back(this);
}

GnsTransport::~GnsTransport() {
    Close();
    auto& v = ActiveTransports();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

bool GnsTransport::Listen(std::uint16_t port) {
    if (!g_gns_ready)
        return false;
    m_is_host = true;
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;
    m_listen = SteamNetworkingSockets()->CreateListenSocketIP(addr, 0, nullptr);
    return m_listen != k_HSteamListenSocket_Invalid;
}

bool GnsTransport::Connect(const std::string& host, std::uint16_t port) {
    if (!g_gns_ready)
        return false;
    m_is_host = false;
    SteamNetworkingIPAddr addr;
    addr.Clear();
    char hostport[128];
    std::snprintf(hostport, sizeof(hostport), "%s:%u", host.c_str(), static_cast<unsigned>(port));
    if (!addr.ParseString(hostport)) {
        LUMINUMBRA_CORE_ERROR("GnsTransport: bad address '{}'", hostport);
        return false;
    }
    m_conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    return m_conn != k_HSteamNetConnection_Invalid;
}

void GnsTransport::OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info) {
    for (GnsTransport* t : ActiveTransports())
        t->HandleConnStatusChanged(info);
}

void GnsTransport::HandleConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
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

bool GnsTransport::SendFrame(const std::vector<std::uint8_t>& frame, FrameDelivery delivery) {
    if (!g_gns_ready || m_conn == k_HSteamNetConnection_Invalid)
        return false;
    const int flags = (delivery == FrameDelivery::Reliable) ? k_nSteamNetworkingSend_Reliable
                                                            : k_nSteamNetworkingSend_Unreliable;
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, frame.data(), static_cast<std::uint32_t>(frame.size()), flags, nullptr);
    return r == k_EResultOK;
}

void GnsTransport::DrainInto() {
    if (!g_gns_ready || m_conn == k_HSteamNetConnection_Invalid)
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

bool GnsTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    DrainInto();
    if (m_recv.empty())
        return false;
    out = std::move(m_recv.front());
    m_recv.pop_front();
    return true;
}

bool GnsTransport::IsPeerConnected() const {
    return m_connected && !m_closed;
}

void GnsTransport::Close() {
    if (m_conn != k_HSteamNetConnection_Invalid && g_gns_ready) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "closing", true);
    }
    m_conn = k_HSteamNetConnection_Invalid;
    if (m_listen != k_HSteamListenSocket_Invalid && g_gns_ready) {
        SteamNetworkingSockets()->CloseListenSocket(m_listen);
    }
    m_listen = k_HSteamListenSocket_Invalid;
    m_connected = false;
}

} // namespace Luminumbra::Net

#endif // LUMINUMBRA_ENABLE_GNS
