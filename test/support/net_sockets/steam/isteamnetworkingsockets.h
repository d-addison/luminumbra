#pragma once

#include "steamnetworkingtypes.h"
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Fake the SDK boundary, leaving the production transport and receive queue intact.
class FakeNetworkingSockets {
public:
    struct IncomingMessage {
        std::vector<std::uint8_t> bytes;
        int flags = k_nSteamNetworkingSend_Unreliable;
        // Override only to exercise rejection before payload access or allocation.
        std::optional<int> reported_size = std::nullopt;
    };

    EResult result = k_EResultOK;
    unsigned send_calls = 0;
    unsigned close_calls = 0;
    unsigned released_messages = 0;
    std::deque<IncomingMessage> incoming;
    std::vector<std::vector<std::uint8_t>> attempts;
    std::vector<int> attempt_flags;
    std::string close_reason;

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
    bool CloseConnection(HSteamNetConnection, int, const char* reason, bool) {
        ++close_calls;
        close_reason = reason;
        return true;
    }
    EResult SendMessageToConnection(
        HSteamNetConnection, const void* data, std::uint32_t size, int flags, std::int64_t*) {
        ++send_calls;
        attempt_flags.push_back(flags);
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
        message->data = std::move(incoming.front().bytes);
        message->m_nFlags = incoming.front().flags;
        message->m_cbSize =
            incoming.front().reported_size.value_or(static_cast<int>(message->data.size()));
        incoming.pop_front();
        message->owner = this;
        message->m_pData = message->data.data();
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
