#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luminumbra::network {

struct NetworkLoopbackInput {
    std::string clientId;
    std::uint32_t tick = 0;
    std::uint32_t sequence = 0;
    int throttleMmPerTick = 0;
    int strafeMmPerTick = 0;
    bool clientAuthorityClaim = false;
};

struct NetworkLoopbackState {
    std::uint32_t tick = 0;
    std::uint32_t authoritativeRevision = 0;
    int positionXMm = 0;
    int positionYMm = 0;
};

struct NetworkLoopbackDecision {
    std::string clientId;
    std::uint32_t tick = 0;
    std::uint32_t sequence = 0;
    bool accepted = false;
    std::string reason;
    NetworkLoopbackState authoritativeState;
};

struct NetworkLoopbackCheck {
    std::string name;
    bool passed = false;
};

struct NetworkLoopbackConvergenceReport {
    std::string schema;
    bool passed = false;
    std::string buildPreset;

    std::string source;
    std::string header;
    std::string serializer;
    std::string validationApi;
    std::string artifactWriter;
    std::string authorityContract;
    std::string orderContract;

    std::string transport;
    std::string simulation;
    std::string authoritativeClientId;
    std::uint32_t submittedFrameCount = 0;
    std::uint32_t acceptedFrameCount = 0;
    std::uint32_t rejectedFrameCount = 0;
    bool unauthorizedAuthorityClaimRejected = false;
    bool clientPredictionReconciled = false;
    bool converged = false;
    std::uint32_t convergenceTick = 0;
    int predictionErrorBeforeReconcileMm = 0;
    int predictionErrorAfterReconcileMm = 0;
    std::string authoritativeChecksum;

    NetworkLoopbackState finalAuthoritativeState;
    NetworkLoopbackState reconciledClientState;
    std::vector<NetworkLoopbackDecision> decisions;
    std::vector<NetworkLoopbackCheck> checks;
};

struct NetworkMultiClientAcceptCheck {
    std::string name;
    bool passed = false;
};

struct NetworkMultiClientAcceptReport {
    std::string schema;
    bool passed = false;
    std::string source;
    std::string header;
    std::string portMappingApi;
    std::string validationApi;
    std::string artifactWriter;
    std::string acceptContract;
    std::uint32_t expectedClientCount = 0;
    std::uint32_t firstClientId = 0;
    std::uint32_t lastClientId = 0;
    std::uint16_t basePort = 0;
    std::vector<std::uint16_t> tcpAcceptPorts;
    std::vector<std::uint16_t> udpAcceptPorts;
    bool tcpAcceptsAllExpectedClients = false;
    bool udpAcceptsAllExpectedClients = false;
    bool uniquePlayerIds = false;
    bool deterministicPortMapping = false;
    std::vector<NetworkMultiClientAcceptCheck> checks;
};

NetworkLoopbackConvergenceReport BuildNetworkLoopbackConvergenceFixture(
    const std::string& buildPreset = "debug");

std::string SerializeNetworkLoopbackConvergenceJson(
    const NetworkLoopbackConvergenceReport& report);

bool NetworkLoopbackAuthorityMeetsBaseline(
    const NetworkLoopbackConvergenceReport& report);

bool WriteNetworkLoopbackConvergenceArtifact(
    const std::string& path,
    const std::string& buildPreset = "debug");

bool TryNetworkMultiClientAcceptPortForClient(
    std::uint16_t basePort,
    std::uint32_t clientId,
    std::uint16_t& outPort);

NetworkMultiClientAcceptReport BuildNetworkMultiClientAcceptFixture(
    std::uint32_t expectedClientCount = 2,
    std::uint16_t basePort = 27015);

std::string SerializeNetworkMultiClientAcceptJson(
    const NetworkMultiClientAcceptReport& report);

bool NetworkMultiClientAcceptMeetsBaseline(
    const NetworkMultiClientAcceptReport& report);

bool WriteNetworkMultiClientAcceptArtifact(
    const std::string& path,
    std::uint32_t expectedClientCount = 2,
    std::uint16_t basePort = 27015);

} // namespace luminumbra::network
