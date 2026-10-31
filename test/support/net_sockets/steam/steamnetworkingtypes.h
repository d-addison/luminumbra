#pragma once

// Minimal public SDK surface for the isolated transport contract target.
#include <cstdint>

using HSteamListenSocket = std::uint32_t;
using HSteamNetConnection = std::uint32_t;
inline constexpr HSteamListenSocket k_HSteamListenSocket_Invalid = 0;
inline constexpr HSteamNetConnection k_HSteamNetConnection_Invalid = 0;
inline constexpr int k_cchSteamNetworkingMaxConnectionCloseReason = 128;
inline constexpr int k_cbMaxSteamNetworkingSocketsMessageSizeSend = 512 * 1024;
inline constexpr int k_nSteamNetworkingSend_Reliable = 8;
inline constexpr int k_nSteamNetworkingSend_Unreliable = 0;

enum EResult {
    k_EResultOK = 1,
    k_EResultFail = 2,
    k_EResultNoConnection = 3,
    k_EResultInvalidParam = 8,
    k_EResultInvalidState = 11,
    k_EResultLimitExceeded = 25,
    k_EResultIgnored = 41,
};
enum ESteamNetworkingConnectionState {
    k_ESteamNetworkingConnectionState_Connecting = 1,
    k_ESteamNetworkingConnectionState_Connected = 3,
    k_ESteamNetworkingConnectionState_ClosedByPeer = 4,
    k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,
};
struct SteamNetworkingIPAddr {
    std::uint16_t m_port = 0;
    void Clear() {
        m_port = 0;
    }
    bool ParseString(const char*) {
        return true;
    }
};
struct SteamNetConnectionInfo_t {
    HSteamListenSocket m_hListenSocket = 0;
    ESteamNetworkingConnectionState m_eState = k_ESteamNetworkingConnectionState_Connecting;
    int m_eEndReason = 0;
    // NUL-terminated diagnostic text, matching the public SDK buffer and bound.
    char m_szEndDebug[k_cchSteamNetworkingMaxConnectionCloseReason]{};
};
struct SteamNetConnectionStatusChangedCallback_t {
    HSteamNetConnection m_hConn = 0;
    SteamNetConnectionInfo_t m_info;
};
struct SteamNetworkingMessage_t {
    void* m_pData = nullptr;
    int m_cbSize = 0;
    // The reliable bit describes delivery of this received message.
    int m_nFlags = k_nSteamNetworkingSend_Unreliable;
    void (*m_pfnRelease)(SteamNetworkingMessage_t*) = nullptr;
    void Release() {
        m_pfnRelease(this);
    }
};
