#pragma once

#include "steamnetworkingtypes.h"

class FakeNetworkingUtils {
public:
    void SetGlobalCallback_SteamNetConnectionStatusChanged(
        void (*)(SteamNetConnectionStatusChangedCallback_t*)) {}
};

inline FakeNetworkingUtils* SteamNetworkingUtils() {
    static FakeNetworkingUtils utils;
    return &utils;
}
