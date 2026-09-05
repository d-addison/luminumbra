#include "NetSocketsTransport.h"

#if defined(LUMINUMBRA_ENABLE_STEAM) || defined(LUMINUMBRA_ENABLE_GNS)

#include <algorithm>
#include <cstdio>
#include <vector>

#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

#include "../core/Log.h"

namespace Luminumbra::Net {

namespace {
// v1: single peer per transport. The global connection-status callback is
// process-wide, so it fans out to every live transport; each one claims the
// events for its own listen socket / connection.
std::vector<NetSocketsTransport*>& ActiveTransports() {
    static std::vector<NetSocketsTransport*> s;
    return s;
}
} // namespace

bool InitializeNetSocketsBackend(NetSocketsBackend& backend, std::uint32_t argument) {
    if (backend.ready)
        return true;
    if (!backend.initialize(argument))
        return false;
    // Route connection-status changes (accept/connect/close) to our static dispatcher.
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
        &NetSocketsTransport::OnConnStatusChangedStatic);
    backend.ready = true;
    return true;
}

void ShutdownNetSocketsBackend(NetSocketsBackend& backend) {
    if (!backend.ready)
        return;
    backend.shutdown();
    backend.ready = false;
}

void RunNetSocketsBackendCallbacks(NetSocketsBackend& backend) {
    if (backend.ready)
        backend.run_callbacks();
}

NetSocketsTransport::NetSocketsTransport(NetSocketsBackend& backend, const char* name)
    : m_backend(backend)
    , m_name(name) {
    ActiveTransports().push_back(this);
}

NetSocketsTransport::~NetSocketsTransport() {
    Close();
    auto& v = ActiveTransports();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

bool NetSocketsTransport::Listen(std::uint16_t port) {
    if (!m_backend.ready)
        return false;
    m_is_host = true;
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port; // any interface, given port
    m_listen = SteamNetworkingSockets()->CreateListenSocketIP(addr, 0, nullptr);
    return m_listen != k_HSteamListenSocket_Invalid;
}

bool NetSocketsTransport::Connect(const std::string& host, std::uint16_t port) {
    if (!m_backend.ready)
        return false;
    m_is_host = false;
    SteamNetworkingIPAddr addr;
    addr.Clear();
    char hostport[128];
    std::snprintf(hostport, sizeof(hostport), "%s:%u", host.c_str(), static_cast<unsigned>(port));
    if (!addr.ParseString(hostport)) {
        LUMINUMBRA_CORE_ERROR("{}: bad address '{}'", m_name, hostport);
        return false;
    }
    m_conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    return m_conn != k_HSteamNetConnection_Invalid;
}

void NetSocketsTransport::OnConnStatusChangedStatic(
    SteamNetConnectionStatusChangedCallback_t* info) {
    for (NetSocketsTransport* transport : ActiveTransports()) {
        transport->HandleConnStatusChanged(info);
    }
}

void NetSocketsTransport::HandleConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
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

bool NetSocketsTransport::SendFrame(const std::vector<std::uint8_t>& frame,
                                    FrameDelivery delivery) {
    if (!m_backend.ready || m_conn == k_HSteamNetConnection_Invalid)
        return false;
    const int flags = (delivery == FrameDelivery::Reliable) ? k_nSteamNetworkingSend_Reliable
                                                            : k_nSteamNetworkingSend_Unreliable;
    const EResult result = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, frame.data(), static_cast<std::uint32_t>(frame.size()), flags, nullptr);
    return result == k_EResultOK;
}

void NetSocketsTransport::DrainInto() {
    if (!m_backend.ready || m_conn == k_HSteamNetConnection_Invalid)
        return;
    SteamNetworkingMessage_t* messages[32];
    const int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_conn, messages, 32);
    for (int i = 0; i < count; ++i) {
        SteamNetworkingMessage_t* message = messages[i];
        const auto* data = static_cast<const std::uint8_t*>(message->m_pData);
        m_recv.emplace_back(data, data + message->m_cbSize);
        message->Release();
    }
}

bool NetSocketsTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    DrainInto();
    if (m_recv.empty())
        return false;
    out = std::move(m_recv.front());
    m_recv.pop_front();
    return true;
}

bool NetSocketsTransport::IsPeerConnected() const {
    return m_connected && !m_closed;
}

void NetSocketsTransport::Close() {
    if (m_conn != k_HSteamNetConnection_Invalid && m_backend.ready) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "closing", true);
    }
    m_conn = k_HSteamNetConnection_Invalid;
    if (m_listen != k_HSteamListenSocket_Invalid && m_backend.ready) {
        SteamNetworkingSockets()->CloseListenSocket(m_listen);
    }
    m_listen = k_HSteamListenSocket_Invalid;
    m_connected = false;
}

} // namespace Luminumbra::Net

#endif // defined(LUMINUMBRA_ENABLE_STEAM) || defined(LUMINUMBRA_ENABLE_GNS)
