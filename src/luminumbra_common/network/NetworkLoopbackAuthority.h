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

struct NetworkRuntimeJoinLeaveEvent {
    std::uint32_t tick = 0;
    std::uint32_t clientId = 0;
    std::string event;
    bool hostContinued = false;
};

struct NetworkRuntimeJoinLeaveClient {
    std::uint32_t clientId = 0;
    std::uint16_t acceptPort = 0;
    std::uint32_t joinedAtTick = 0;
    std::uint32_t leftAtTick = 0;
    std::uint32_t finalAckedSnapshotSeq = 0;
};

struct NetworkRuntimeJoinLeaveCheck {
    std::string name;
    bool passed = false;
};

struct NetworkRuntimeJoinLeaveReport {
    std::string schema;
    bool passed = false;
    std::string source;
    std::string header;
    std::string portMappingApi;
    std::string validationApi;
    std::string artifactWriter;
    std::string serverModeContract;
    std::uint32_t expectedClientCount = 0;
    std::uint32_t ticksExecuted = 0;
    std::uint16_t basePort = 0;
    bool emptyServerTicksBeforeJoin = false;
    bool lateJoinAccepted = false;
    bool leaveDoesNotStopHost = false;
    bool hostRunsAfterLastLeave = false;
    bool stablePlayerIds = false;
    bool deterministicPortMapping = false;
    std::vector<NetworkRuntimeJoinLeaveClient> clients;
    std::vector<NetworkRuntimeJoinLeaveEvent> events;
    std::vector<NetworkRuntimeJoinLeaveCheck> checks;
};

struct NetworkRemoteAvatarRenderPose {
    std::uint32_t clientId = 0;
    std::uint32_t serverTick = 0;
    std::uint32_t snapshotSequence = 0;
    int positionXMm = 0;
    int positionYMm = 0;
    int positionZMm = 0;
    bool remote = false;
    bool interpolated = false;
    bool rendered = false;
};

struct NetworkRemoteAvatarRenderCheck {
    std::string name;
    bool passed = false;
};

struct NetworkRemoteAvatarRenderReport {
    std::string schema;
    bool passed = false;
    std::string source;
    std::string header;
    std::string builderApi;
    std::string validationApi;
    std::string artifactWriter;
    std::string replicationContract;
    std::string interpolationContract;
    std::string renderContract;
    std::uint32_t localClientId = 0;
    std::uint32_t expectedAvatarCount = 0;
    std::uint32_t remoteAvatarCount = 0;
    std::uint32_t snapshotFrameCount = 0;
    std::uint32_t renderedAvatarCount = 0;
    std::uint32_t skinnedDraws = 0;
    std::uint32_t skinnedIndicesDrawn = 0;
    bool serverSnapshotsReceived = false;
    bool remoteAvatarsInterpolated = false;
    bool remoteAvatarsRendered = false;
    bool localAvatarExcludedFromRemoteSet = false;
    bool deterministicClientOrdering = false;
    std::vector<NetworkRemoteAvatarRenderPose> poses;
    std::vector<NetworkRemoteAvatarRenderCheck> checks;
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

NetworkRuntimeJoinLeaveReport BuildNetworkRuntimeJoinLeaveFixture(
    std::uint32_t expectedClientCount = 2,
    std::uint16_t basePort = 27015,
    std::uint32_t ticksExecuted = 12);

std::string SerializeNetworkRuntimeJoinLeaveJson(
    const NetworkRuntimeJoinLeaveReport& report);

bool NetworkRuntimeJoinLeaveMeetsBaseline(
    const NetworkRuntimeJoinLeaveReport& report);

bool WriteNetworkRuntimeJoinLeaveArtifact(
    const std::string& path,
    std::uint32_t expectedClientCount = 2,
    std::uint16_t basePort = 27015,
    std::uint32_t ticksExecuted = 12);

NetworkRemoteAvatarRenderReport BuildNetworkRemoteAvatarRenderReport(
    std::uint32_t localClientId,
    const std::vector<NetworkRemoteAvatarRenderPose>& poses,
    std::uint32_t expectedAvatarCount,
    std::uint32_t snapshotFrameCount,
    std::uint32_t renderedAvatarCount,
    std::uint32_t skinnedDraws,
    std::uint32_t skinnedIndicesDrawn);

NetworkRemoteAvatarRenderReport BuildNetworkRemoteAvatarRenderFixture(
    std::uint32_t expectedAvatarCount = 3,
    std::uint32_t renderedAvatarCount = 3,
    std::uint32_t skinnedDraws = 3,
    std::uint32_t skinnedIndicesDrawn = 900);

std::string SerializeNetworkRemoteAvatarRenderJson(
    const NetworkRemoteAvatarRenderReport& report);

bool NetworkRemoteAvatarRenderMeetsBaseline(
    const NetworkRemoteAvatarRenderReport& report);

bool WriteNetworkRemoteAvatarRenderArtifact(
    const std::string& path,
    const NetworkRemoteAvatarRenderReport& report);

bool WriteNetworkRemoteAvatarRenderFixtureArtifact(
    const std::string& path,
    std::uint32_t expectedAvatarCount = 3,
    std::uint32_t renderedAvatarCount = 3,
    std::uint32_t skinnedDraws = 3,
    std::uint32_t skinnedIndicesDrawn = 900);

} // namespace luminumbra::network
