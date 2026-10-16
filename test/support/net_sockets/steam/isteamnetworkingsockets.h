#pragma once

#include "steamnetworkingtypes.h"
#include <deque>
#include <utility>
#include <vector>

// Fake the SDK boundary, leaving the production transport and receive queue intact.
class FakeNetworkingSockets {
public:
    EResult result = k_EResultOK;
    unsigned send_calls = 0;
    unsigned close_calls = 0;
    unsigned released_messages = 0;
    std::deque<std::vector<std::uint8_t>> incoming;
    std::vector<std::vector<std::uint8_t>> attempts;

    HSteamListenSocket CreateListenSocketIP(const SteamNetworkingIPAddr&, int, const void*) {
        return 1;
    }
    HSteamNetConnection ConnectByIPAddress(const SteamNetworkingIPAddr&, int, const void*) {
        return 2;
    }
    EResult AcceptConnection(HSteamNetConnection) {
        return k_EResultOK;
    }
    bool CloseListenSocket(HSteamListenSocket) {
        return true;
    }
    bool CloseConnection(HSteamNetConnection, int, const char*, bool) {
        ++close_calls;
        return true;
    }
    EResult SendMessageToConnection(
        HSteamNetConnection, const void* data, std::uint32_t size, int, std::int64_t*) {
        ++send_calls;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (size == 0)
            attempts.emplace_back();
        else
            attempts.emplace_back(bytes, bytes + size);
        return result;
    }
    int ReceiveMessagesOnConnection(HSteamNetConnection, SteamNetworkingMessage_t** out, int) {
        if (incoming.empty())
            return 0;
        auto* message = new OwnedMessage;
        message->data = std::move(incoming.front());
        incoming.pop_front();
        message->owner = this;
        message->m_pData = message->data.data();
        message->m_cbSize = static_cast<int>(message->data.size());
        message->m_pfnRelease = [](SteamNetworkingMessage_t* base) {
            auto* owned = static_cast<OwnedMessage*>(base);
            ++owned->owner->released_messages;
            delete owned;
        };
        *out = message;
        return 1;
    }

private:
    struct OwnedMessage : SteamNetworkingMessage_t {
        std::vector<std::uint8_t> data;
        FakeNetworkingSockets* owner = nullptr;
    };
};

inline FakeNetworkingSockets* SteamNetworkingSockets() {
    static FakeNetworkingSockets sockets;
    return &sockets;
}
