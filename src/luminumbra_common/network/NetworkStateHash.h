#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luminumbra::network {

struct NetworkStateHashTick {
    std::uint32_t tick = 0;
    std::uint32_t authoritativeRevision = 0;
    int positionXMm = 0;
    int positionYMm = 0;
    std::string canonicalState;
    std::string stateHash;
};

struct NetworkStateHashCheck {
    std::string name;
    bool passed = false;
};

struct NetworkStateHashReport {
    std::string schema;
    bool passed = false;
    std::string buildPreset;

    std::string source;
    std::string header;
    std::string hashApi;
    std::string validationApi;
    std::string artifactWriter;
    std::string stateContract;
    std::string orderContract;
    std::string hashAlgorithm;

    std::string worldHash;
    std::vector<std::string> durableEntityIds;
    std::uint32_t tickCount = 0;
    bool deterministicReplay = false;
    bool monotonicTicks = false;
    std::string finalStateHash;
    std::string replayFinalStateHash;

    std::vector<NetworkStateHashTick> ticks;
    std::vector<NetworkStateHashCheck> checks;
};

NetworkStateHashReport BuildNetworkStateHashFixture(
    const std::string& buildPreset = "debug");

std::string SerializeNetworkStateHashJson(const NetworkStateHashReport& report);

bool NetworkStateHashMeetsBaseline(const NetworkStateHashReport& report);

bool WriteNetworkStateHashArtifact(
    const std::string& path,
    const std::string& buildPreset = "debug");

} // namespace luminumbra::network
