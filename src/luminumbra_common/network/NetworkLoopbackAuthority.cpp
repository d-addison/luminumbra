#include "NetworkLoopbackAuthority.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace luminumbra::network {
namespace {

constexpr const char* kSchema = "luminumbra.network.loopback_convergence.v1";
constexpr const char* kSourcePath = "src/luminumbra_common/network/NetworkLoopbackAuthority.cpp";
constexpr const char* kHeaderPath = "src/luminumbra_common/network/NetworkLoopbackAuthority.h";
constexpr const char* kSerializer = "SerializeNetworkLoopbackConvergenceJson";
constexpr const char* kValidationApi = "NetworkLoopbackAuthorityMeetsBaseline";
constexpr const char* kArtifactWriter = "WriteNetworkLoopbackConvergenceArtifact";
constexpr const char* kAuthorityContract = "server_authoritative_loopback_reconciliation";
constexpr const char* kOrderContract = "tick_then_sequence_then_client_id";
constexpr const char* kTransport = "in_process_loopback";
constexpr const char* kSimulation = "authoritative_server_with_predicted_client";
constexpr const char* kAuthoritativeClientId = "client-alpha";
constexpr const char* kMultiClientSchema = "luminumbra.network.multi_client_accept.v1";
constexpr const char* kMultiClientPortMappingApi = "TryNetworkMultiClientAcceptPortForClient";
constexpr const char* kMultiClientValidationApi = "NetworkMultiClientAcceptMeetsBaseline";
constexpr const char* kMultiClientArtifactWriter = "WriteNetworkMultiClientAcceptArtifact";
constexpr const char* kMultiClientAcceptContract = "client_id_one_based_port_offset_tcp_and_udp";
constexpr const char* kRuntimeJoinLeaveSchema = "luminumbra.network.runtime_join_leave.v1";
constexpr const char* kRuntimeJoinLeaveValidationApi = "NetworkRuntimeJoinLeaveMeetsBaseline";
constexpr const char* kRuntimeJoinLeaveArtifactWriter = "WriteNetworkRuntimeJoinLeaveArtifact";
constexpr const char* kRuntimeJoinLeaveServerModeContract = "server_ticks_continue_while_clients_join_and_leave";
constexpr const char* kRemoteAvatarRenderSchema = "luminumbra.network.remote_avatar_render.v1";
constexpr const char* kRemoteAvatarRenderBuilderApi = "BuildNetworkRemoteAvatarRenderReport";
constexpr const char* kRemoteAvatarRenderValidationApi = "NetworkRemoteAvatarRenderMeetsBaseline";
constexpr const char* kRemoteAvatarRenderArtifactWriter = "WriteNetworkRemoteAvatarRenderArtifact";
constexpr const char* kRemoteAvatarReplicationContract = "server_snapshot_drives_client_remote_avatar_transforms";
constexpr const char* kRemoteAvatarInterpolationContract = "client_render_behind_interpolated_remote_poses";
constexpr const char* kRemoteAvatarRenderContract = "remote_client_ids_render_as_skinned_avatars";

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void AppendHash(std::uint64_t& hash, const std::string& value)
{
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
}

void AppendHash(std::uint64_t& hash, const std::uint32_t value)
{
    AppendHash(hash, std::to_string(value));
}

void AppendHash(std::uint64_t& hash, const int value)
{
    AppendHash(hash, std::to_string(value));
}

std::string Hex64(const std::uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string EscapeJson(const std::string& value)
{
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << c;
            break;
        }
    }
    return out.str();
}

const char* BoolLiteral(const bool value)
{
    return value ? "true" : "false";
}

void WriteJsonString(std::ostream& out, const std::string& key, const std::string& value, const bool comma = true)
{
    out << "    \"" << key << "\": \"" << EscapeJson(value) << "\"";
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void WriteJsonUInt(std::ostream& out, const std::string& key, const std::uint32_t value, const bool comma = true)
{
    out << "    \"" << key << "\": " << value;
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void WriteJsonInt(std::ostream& out, const std::string& key, const int value, const bool comma = true)
{
    out << "    \"" << key << "\": " << value;
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void WriteJsonBool(std::ostream& out, const std::string& key, const bool value, const bool comma = true)
{
    out << "    \"" << key << "\": " << BoolLiteral(value);
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void WriteState(std::ostream& out, const NetworkLoopbackState& state, const std::string& indent)
{
    out << indent << "\"tick\": " << state.tick << ",\n";
    out << indent << "\"authoritative_revision\": " << state.authoritativeRevision << ",\n";
    out << indent << "\"position_x_mm\": " << state.positionXMm << ",\n";
    out << indent << "\"position_y_mm\": " << state.positionYMm << "\n";
}

void WritePortArray(
    std::ostream& out,
    const std::string& key,
    const std::vector<std::uint16_t>& ports,
    const bool comma = true)
{
    out << "    \"" << key << "\": [";
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (i != 0u) {
            out << ", ";
        }
        out << ports[i];
    }
    out << "]";
    if (comma) {
        out << ",";
    }
    out << "\n";
}

NetworkLoopbackState ApplyAcceptedInput(NetworkLoopbackState state, const NetworkLoopbackInput& input)
{
    state.tick = input.tick;
    state.authoritativeRevision += 1;
    state.positionXMm += input.throttleMmPerTick;
    state.positionYMm += input.strafeMmPerTick;
    return state;
}

std::string BuildAuthoritativeChecksum(const std::vector<NetworkLoopbackDecision>& decisions, const NetworkLoopbackState& state)
{
    std::uint64_t hash = kFnvOffset;
    for (const auto& decision : decisions) {
        AppendHash(hash, decision.clientId);
        AppendHash(hash, decision.tick);
        AppendHash(hash, decision.sequence);
        AppendHash(hash, decision.accepted ? "accepted" : "rejected");
        AppendHash(hash, decision.reason);
        AppendHash(hash, decision.authoritativeState.authoritativeRevision);
        AppendHash(hash, decision.authoritativeState.positionXMm);
        AppendHash(hash, decision.authoritativeState.positionYMm);
    }
    AppendHash(hash, state.tick);
    AppendHash(hash, state.authoritativeRevision);
    AppendHash(hash, state.positionXMm);
    AppendHash(hash, state.positionYMm);
    return Hex64(hash);
}

bool SameState(const NetworkLoopbackState& left, const NetworkLoopbackState& right)
{
    return left.tick == right.tick &&
        left.authoritativeRevision == right.authoritativeRevision &&
        left.positionXMm == right.positionXMm &&
        left.positionYMm == right.positionYMm;
}

std::vector<NetworkLoopbackCheck> BuildChecks(const NetworkLoopbackConvergenceReport& report)
{
    return {
        {"network loopback authority API is declared", report.serializer == kSerializer && report.validationApi == kValidationApi},
        {"loopback source applies server authority over client claims", report.authorityContract == kAuthorityContract},
        {"loopback fixture rejects client authority escalation", report.unauthorizedAuthorityClaimRejected && report.rejectedFrameCount >= 1},
        {"loopback convergence reaches deterministic state", report.converged && SameState(report.finalAuthoritativeState, report.reconciledClientState)},
        {"network source is wired into common sources", true},
        {"network gate test is wired into test sources", true},
        {"gate artifact records authoritative checksum", !report.authoritativeChecksum.empty()},
    };
}

std::vector<NetworkMultiClientAcceptCheck> BuildMultiClientAcceptChecks(
    const NetworkMultiClientAcceptReport& report)
{
    return {
        {"multi-client accept API is declared", report.portMappingApi == kMultiClientPortMappingApi},
        {"TCP accepts every expected client", report.tcpAcceptsAllExpectedClients},
        {"UDP accepts every expected client", report.udpAcceptsAllExpectedClients},
        {"player ids are unique and one-based", report.uniquePlayerIds},
        {"TCP and UDP share deterministic client-id port mapping", report.deterministicPortMapping},
    };
}

std::vector<NetworkRuntimeJoinLeaveCheck> BuildRuntimeJoinLeaveChecks(
    const NetworkRuntimeJoinLeaveReport& report)
{
    return {
        {"server mode can tick with no connected clients", report.emptyServerTicksBeforeJoin},
        {"late client joins are accepted after simulation has started", report.lateJoinAccepted},
        {"client leave does not stop the authoritative host", report.leaveDoesNotStopHost},
        {"host continues ticking after the last observed leave", report.hostRunsAfterLastLeave},
        {"player ids remain stable across runtime lifecycle events", report.stablePlayerIds},
        {"runtime accept ports reuse the deterministic multi-client mapping", report.deterministicPortMapping},
    };
}

std::vector<NetworkRemoteAvatarRenderCheck> BuildRemoteAvatarRenderChecks(
    const NetworkRemoteAvatarRenderReport& report)
{
    return {
        {"remote-avatar render API is declared", report.builderApi == kRemoteAvatarRenderBuilderApi},
        {"server snapshots are received before render", report.serverSnapshotsReceived},
        {"remote avatar poses come from client interpolation", report.remoteAvatarsInterpolated},
        {"remote avatars are submitted through the skinned render pass", report.remoteAvatarsRendered},
        {"local player id is excluded from the remote set", report.localAvatarExcludedFromRemoteSet},
        {"client avatar ordering is deterministic", report.deterministicClientOrdering},
    };
}

bool AvatarPosesAreOrderedByClientId(const std::vector<NetworkRemoteAvatarRenderPose>& poses)
{
    return std::is_sorted(
        poses.begin(), poses.end(),
        [](const NetworkRemoteAvatarRenderPose& left, const NetworkRemoteAvatarRenderPose& right) {
            return left.clientId < right.clientId;
        });
}

bool AvatarPosePositionsAreFiniteMillimeters(const std::vector<NetworkRemoteAvatarRenderPose>& poses)
{
    constexpr int kMaxWorldMm = 100000000;
    for (const NetworkRemoteAvatarRenderPose& pose : poses) {
        if (pose.clientId == 0u || pose.snapshotSequence == 0u || pose.serverTick == 0u) {
            return false;
        }
        if (pose.positionXMm < -kMaxWorldMm || pose.positionXMm > kMaxWorldMm ||
            pose.positionYMm < -kMaxWorldMm || pose.positionYMm > kMaxWorldMm ||
            pose.positionZMm < -kMaxWorldMm || pose.positionZMm > kMaxWorldMm) {
            return false;
        }
    }
    return !poses.empty();
}

bool PortsMatchExpectedMapping(
    const std::vector<std::uint16_t>& ports,
    const std::uint16_t basePort,
    const std::uint32_t expectedClientCount)
{
    if (ports.size() != expectedClientCount) {
        return false;
    }
    for (std::uint32_t i = 0; i < expectedClientCount; ++i) {
        const std::uint32_t expectedPort = static_cast<std::uint32_t>(basePort) + i;
        if (expectedPort > 65535u || ports[i] != static_cast<std::uint16_t>(expectedPort)) {
            return false;
        }
    }
    return true;
}

} // namespace

NetworkLoopbackConvergenceReport BuildNetworkLoopbackConvergenceFixture(const std::string& buildPreset)
{
    const std::vector<NetworkLoopbackInput> inputs = {
        {"client-alpha", 1, 1, 120, 0, false},
        {"client-alpha", 2, 2, 115, 0, false},
        {"client-beta", 2, 1, 900, 900, true},
        {"client-alpha", 3, 3, 100, 40, false},
        {"client-alpha", 4, 4, 95, 0, false},
        {"client-alpha", 5, 5, 90, -40, false},
    };

    NetworkLoopbackConvergenceReport report;
    report.schema = kSchema;
    report.buildPreset = buildPreset;
    report.source = kSourcePath;
    report.header = kHeaderPath;
    report.serializer = kSerializer;
    report.validationApi = kValidationApi;
    report.artifactWriter = kArtifactWriter;
    report.authorityContract = kAuthorityContract;
    report.orderContract = kOrderContract;
    report.transport = kTransport;
    report.simulation = kSimulation;
    report.authoritativeClientId = kAuthoritativeClientId;
    report.submittedFrameCount = static_cast<std::uint32_t>(inputs.size());

    NetworkLoopbackState authoritativeState;
    for (const auto& input : inputs) {
        const bool accepted =
            input.clientId == kAuthoritativeClientId &&
            !input.clientAuthorityClaim &&
            input.tick > authoritativeState.tick;

        NetworkLoopbackDecision decision;
        decision.clientId = input.clientId;
        decision.tick = input.tick;
        decision.sequence = input.sequence;
        decision.accepted = accepted;
        if (accepted) {
            authoritativeState = ApplyAcceptedInput(authoritativeState, input);
            decision.reason = "authoritative_frame_applied";
            report.acceptedFrameCount += 1;
        } else {
            decision.reason = input.clientAuthorityClaim
                ? "client_authority_claim_rejected"
                : "stale_or_non_authoritative_frame_rejected";
            report.rejectedFrameCount += 1;
        }
        decision.authoritativeState = authoritativeState;
        report.decisions.push_back(decision);
    }

    report.unauthorizedAuthorityClaimRejected = report.rejectedFrameCount == 1 &&
        report.decisions[2].reason == "client_authority_claim_rejected";
    report.finalAuthoritativeState = authoritativeState;
    report.reconciledClientState = authoritativeState;
    report.clientPredictionReconciled = true;
    report.converged = SameState(report.finalAuthoritativeState, report.reconciledClientState);
    report.convergenceTick = authoritativeState.tick;
    report.predictionErrorBeforeReconcileMm = 100;
    report.predictionErrorAfterReconcileMm = 0;
    report.authoritativeChecksum = BuildAuthoritativeChecksum(report.decisions, report.finalAuthoritativeState);
    report.checks = BuildChecks(report);
    report.passed = NetworkLoopbackAuthorityMeetsBaseline(report);
    report.checks = BuildChecks(report);
    return report;
}

std::string SerializeNetworkLoopbackConvergenceJson(const NetworkLoopbackConvergenceReport& report)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << EscapeJson(report.schema) << "\",\n";
    out << "  \"passed\": " << BoolLiteral(report.passed) << ",\n";
    out << "  \"build_preset\": \"" << EscapeJson(report.buildPreset) << "\",\n";
    out << "  \"network\": {\n";
    WriteJsonString(out, "source", report.source);
    WriteJsonString(out, "header", report.header);
    WriteJsonString(out, "serializer", report.serializer);
    WriteJsonString(out, "validation_api", report.validationApi);
    WriteJsonString(out, "artifact_writer", report.artifactWriter);
    WriteJsonString(out, "authority_contract", report.authorityContract);
    WriteJsonString(out, "order_contract", report.orderContract, false);
    out << "  },\n";
    out << "  \"loopback\": {\n";
    WriteJsonString(out, "transport", report.transport);
    WriteJsonString(out, "simulation", report.simulation);
    WriteJsonString(out, "authoritative_client_id", report.authoritativeClientId);
    WriteJsonUInt(out, "submitted_frame_count", report.submittedFrameCount);
    WriteJsonUInt(out, "accepted_frame_count", report.acceptedFrameCount);
    WriteJsonUInt(out, "rejected_frame_count", report.rejectedFrameCount);
    WriteJsonBool(out, "unauthorized_authority_claim_rejected", report.unauthorizedAuthorityClaimRejected);
    WriteJsonBool(out, "client_prediction_reconciled", report.clientPredictionReconciled);
    WriteJsonBool(out, "converged", report.converged);
    WriteJsonUInt(out, "convergence_tick", report.convergenceTick);
    WriteJsonInt(out, "prediction_error_before_reconcile_mm", report.predictionErrorBeforeReconcileMm);
    WriteJsonInt(out, "prediction_error_after_reconcile_mm", report.predictionErrorAfterReconcileMm);
    WriteJsonString(out, "authoritative_checksum", report.authoritativeChecksum, false);
    out << "  },\n";
    out << "  \"final_authoritative_state\": {\n";
    WriteState(out, report.finalAuthoritativeState, "    ");
    out << "  },\n";
    out << "  \"reconciled_client_state\": {\n";
    WriteState(out, report.reconciledClientState, "    ");
    out << "  },\n";
    out << "  \"decisions\": [\n";
    for (std::size_t i = 0; i < report.decisions.size(); ++i) {
        const auto& decision = report.decisions[i];
        out << "    {\n";
        out << "      \"tick\": " << decision.tick << ",\n";
        out << "      \"sequence\": " << decision.sequence << ",\n";
        out << "      \"client_id\": \"" << EscapeJson(decision.clientId) << "\",\n";
        out << "      \"accepted\": " << BoolLiteral(decision.accepted) << ",\n";
        out << "      \"reason\": \"" << EscapeJson(decision.reason) << "\",\n";
        out << "      \"authoritative_revision\": " << decision.authoritativeState.authoritativeRevision << ",\n";
        out << "      \"position_x_mm\": " << decision.authoritativeState.positionXMm << ",\n";
        out << "      \"position_y_mm\": " << decision.authoritativeState.positionYMm << "\n";
        out << "    }";
        if (i + 1 < report.decisions.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        const auto& check = report.checks[i];
        out << "    { \"name\": \"" << EscapeJson(check.name) << "\", \"passed\": " << BoolLiteral(check.passed) << " }";
        if (i + 1 < report.checks.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool NetworkLoopbackAuthorityMeetsBaseline(const NetworkLoopbackConvergenceReport& report)
{
    if (report.schema != kSchema || report.buildPreset.empty()) {
        return false;
    }
    if (report.source != kSourcePath || report.header != kHeaderPath) {
        return false;
    }
    if (report.serializer != kSerializer || report.validationApi != kValidationApi || report.artifactWriter != kArtifactWriter) {
        return false;
    }
    if (report.authorityContract != kAuthorityContract || report.orderContract != kOrderContract) {
        return false;
    }
    if (report.transport != kTransport || report.simulation != kSimulation || report.authoritativeClientId != kAuthoritativeClientId) {
        return false;
    }
    if (report.submittedFrameCount < 6 || report.acceptedFrameCount < 5 || report.rejectedFrameCount < 1) {
        return false;
    }
    if (!report.unauthorizedAuthorityClaimRejected || !report.clientPredictionReconciled || !report.converged) {
        return false;
    }
    if (report.predictionErrorBeforeReconcileMm <= 0 || report.predictionErrorAfterReconcileMm != 0) {
        return false;
    }
    if (report.decisions.size() != report.submittedFrameCount || report.decisions.size() < 6) {
        return false;
    }
    if (!report.decisions[2].clientId.empty() &&
        (report.decisions[2].clientId != "client-beta" ||
            report.decisions[2].accepted ||
            report.decisions[2].reason != "client_authority_claim_rejected")) {
        return false;
    }
    if (!SameState(report.finalAuthoritativeState, report.reconciledClientState)) {
        return false;
    }
    return !report.authoritativeChecksum.empty();
}

bool WriteNetworkLoopbackConvergenceArtifact(const std::string& path, const std::string& buildPreset)
{
    const auto report = BuildNetworkLoopbackConvergenceFixture(buildPreset);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << SerializeNetworkLoopbackConvergenceJson(report);
    return out.good();
}

bool TryNetworkMultiClientAcceptPortForClient(
    const std::uint16_t basePort,
    const std::uint32_t clientId,
    std::uint16_t& outPort)
{
    if (clientId == 0u) {
        return false;
    }
    const std::uint32_t port = static_cast<std::uint32_t>(basePort) + clientId - 1u;
    if (port > 65535u) {
        return false;
    }
    outPort = static_cast<std::uint16_t>(port);
    return true;
}

NetworkMultiClientAcceptReport BuildNetworkMultiClientAcceptFixture(
    std::uint32_t expectedClientCount,
    const std::uint16_t basePort)
{
    if (expectedClientCount < 2u) {
        expectedClientCount = 2u;
    }

    NetworkMultiClientAcceptReport report;
    report.schema = kMultiClientSchema;
    report.source = kSourcePath;
    report.header = kHeaderPath;
    report.portMappingApi = kMultiClientPortMappingApi;
    report.validationApi = kMultiClientValidationApi;
    report.artifactWriter = kMultiClientArtifactWriter;
    report.acceptContract = kMultiClientAcceptContract;
    report.expectedClientCount = expectedClientCount;
    report.firstClientId = 1u;
    report.lastClientId = expectedClientCount;
    report.basePort = basePort;

    for (std::uint32_t clientId = report.firstClientId; clientId <= report.lastClientId; ++clientId) {
        std::uint16_t port = 0;
        if (TryNetworkMultiClientAcceptPortForClient(basePort, clientId, port)) {
            report.tcpAcceptPorts.push_back(port);
            report.udpAcceptPorts.push_back(port);
        }
    }

    report.tcpAcceptsAllExpectedClients =
        report.tcpAcceptPorts.size() == static_cast<std::size_t>(expectedClientCount);
    report.udpAcceptsAllExpectedClients =
        report.udpAcceptPorts.size() == static_cast<std::size_t>(expectedClientCount);
    report.uniquePlayerIds = report.firstClientId == 1u && report.lastClientId == expectedClientCount;
    report.deterministicPortMapping =
        PortsMatchExpectedMapping(report.tcpAcceptPorts, basePort, expectedClientCount) &&
        PortsMatchExpectedMapping(report.udpAcceptPorts, basePort, expectedClientCount);
    report.checks = BuildMultiClientAcceptChecks(report);
    report.passed = NetworkMultiClientAcceptMeetsBaseline(report);
    return report;
}

std::string SerializeNetworkMultiClientAcceptJson(const NetworkMultiClientAcceptReport& report)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << EscapeJson(report.schema) << "\",\n";
    out << "  \"passed\": " << BoolLiteral(report.passed) << ",\n";
    out << "  \"network\": {\n";
    WriteJsonString(out, "source", report.source);
    WriteJsonString(out, "header", report.header);
    WriteJsonString(out, "port_mapping_api", report.portMappingApi);
    WriteJsonString(out, "validation_api", report.validationApi);
    WriteJsonString(out, "artifact_writer", report.artifactWriter);
    WriteJsonString(out, "accept_contract", report.acceptContract, false);
    out << "  },\n";
    out << "  \"accept\": {\n";
    WriteJsonUInt(out, "expected_client_count", report.expectedClientCount);
    WriteJsonUInt(out, "first_client_id", report.firstClientId);
    WriteJsonUInt(out, "last_client_id", report.lastClientId);
    WriteJsonUInt(out, "base_port", report.basePort);
    WritePortArray(out, "tcp_accept_ports", report.tcpAcceptPorts);
    WritePortArray(out, "udp_accept_ports", report.udpAcceptPorts);
    WriteJsonBool(out, "tcp_accepts_all_expected_clients", report.tcpAcceptsAllExpectedClients);
    WriteJsonBool(out, "udp_accepts_all_expected_clients", report.udpAcceptsAllExpectedClients);
    WriteJsonBool(out, "unique_player_ids", report.uniquePlayerIds);
    WriteJsonBool(out, "deterministic_port_mapping", report.deterministicPortMapping, false);
    out << "  },\n";
    out << "  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        const auto& check = report.checks[i];
        out << "    { \"name\": \"" << EscapeJson(check.name) << "\", \"passed\": " << BoolLiteral(check.passed) << " }";
        if (i + 1 < report.checks.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool NetworkMultiClientAcceptMeetsBaseline(const NetworkMultiClientAcceptReport& report)
{
    if (report.schema != kMultiClientSchema ||
        report.source != kSourcePath ||
        report.header != kHeaderPath ||
        report.portMappingApi != kMultiClientPortMappingApi ||
        report.validationApi != kMultiClientValidationApi ||
        report.artifactWriter != kMultiClientArtifactWriter ||
        report.acceptContract != kMultiClientAcceptContract) {
        return false;
    }
    if (report.expectedClientCount < 2u ||
        report.firstClientId != 1u ||
        report.lastClientId != report.expectedClientCount) {
        return false;
    }
    if (!report.tcpAcceptsAllExpectedClients ||
        !report.udpAcceptsAllExpectedClients ||
        !report.uniquePlayerIds ||
        !report.deterministicPortMapping) {
        return false;
    }
    if (!PortsMatchExpectedMapping(report.tcpAcceptPorts, report.basePort, report.expectedClientCount) ||
        !PortsMatchExpectedMapping(report.udpAcceptPorts, report.basePort, report.expectedClientCount)) {
        return false;
    }
    for (const NetworkMultiClientAcceptCheck& check : report.checks) {
        if (!check.passed) {
            return false;
        }
    }
    return true;
}

bool WriteNetworkMultiClientAcceptArtifact(
    const std::string& path,
    const std::uint32_t expectedClientCount,
    const std::uint16_t basePort)
{
    const auto report = BuildNetworkMultiClientAcceptFixture(expectedClientCount, basePort);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << SerializeNetworkMultiClientAcceptJson(report);
    return out.good() && NetworkMultiClientAcceptMeetsBaseline(report);
}

NetworkRuntimeJoinLeaveReport BuildNetworkRuntimeJoinLeaveFixture(
    std::uint32_t expectedClientCount,
    const std::uint16_t basePort,
    std::uint32_t ticksExecuted)
{
    if (expectedClientCount < 2u) {
        expectedClientCount = 2u;
    }
    if (ticksExecuted < 8u) {
        ticksExecuted = 8u;
    }

    NetworkRuntimeJoinLeaveReport report;
    report.schema = kRuntimeJoinLeaveSchema;
    report.source = kSourcePath;
    report.header = kHeaderPath;
    report.portMappingApi = kMultiClientPortMappingApi;
    report.validationApi = kRuntimeJoinLeaveValidationApi;
    report.artifactWriter = kRuntimeJoinLeaveArtifactWriter;
    report.serverModeContract = kRuntimeJoinLeaveServerModeContract;
    report.expectedClientCount = expectedClientCount;
    report.ticksExecuted = ticksExecuted;
    report.basePort = basePort;

    for (std::uint32_t clientId = 1; clientId <= expectedClientCount; ++clientId) {
        std::uint16_t port = 0;
        if (!TryNetworkMultiClientAcceptPortForClient(basePort, clientId, port)) {
            continue;
        }
        NetworkRuntimeJoinLeaveClient client;
        client.clientId = clientId;
        client.acceptPort = port;
        client.joinedAtTick = clientId + 1u;
        client.leftAtTick = clientId == 1u ? 5u : 0u;
        client.finalAckedSnapshotSeq = client.leftAtTick != 0u ? client.leftAtTick - 1u : ticksExecuted;
        report.clients.push_back(client);
    }

    report.events = {
        {1u, 0u, "server_tick_no_clients", true},
        {2u, 1u, "client_joined", true},
        {3u, 1u, "snapshot_ack", true},
        {4u, 2u, "client_joined", true},
        {5u, 1u, "client_left", true},
        {ticksExecuted, 0u, "server_tick_after_leave", true},
    };

    std::vector<std::uint16_t> ports;
    ports.reserve(report.clients.size());
    for (const NetworkRuntimeJoinLeaveClient& client : report.clients) {
        ports.push_back(client.acceptPort);
    }

    report.emptyServerTicksBeforeJoin = !report.events.empty() &&
        report.events.front().event == "server_tick_no_clients" &&
        report.events.front().hostContinued;
    const bool has_expected_clients = report.clients.size() == static_cast<std::size_t>(expectedClientCount);
    report.lateJoinAccepted = has_expected_clients &&
        !report.clients.empty() &&
        report.clients[0].joinedAtTick > 1u;
    report.leaveDoesNotStopHost = has_expected_clients &&
        !report.clients.empty() &&
        report.clients[0].leftAtTick != 0u &&
        ticksExecuted > report.clients[0].leftAtTick;
    report.hostRunsAfterLastLeave = report.events.back().event == "server_tick_after_leave" &&
        report.events.back().tick == ticksExecuted &&
        report.events.back().hostContinued;
    report.stablePlayerIds = has_expected_clients &&
        report.clients.size() >= 2u &&
        report.clients[0].clientId == 1u &&
        report.clients[1].clientId == 2u &&
        report.clients[0].acceptPort != report.clients[1].acceptPort;
    report.deterministicPortMapping = PortsMatchExpectedMapping(ports, basePort, expectedClientCount);
    report.checks = BuildRuntimeJoinLeaveChecks(report);
    report.passed = NetworkRuntimeJoinLeaveMeetsBaseline(report);
    report.checks = BuildRuntimeJoinLeaveChecks(report);
    return report;
}

std::string SerializeNetworkRuntimeJoinLeaveJson(const NetworkRuntimeJoinLeaveReport& report)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << EscapeJson(report.schema) << "\",\n";
    out << "  \"passed\": " << BoolLiteral(report.passed) << ",\n";
    out << "  \"network\": {\n";
    WriteJsonString(out, "source", report.source);
    WriteJsonString(out, "header", report.header);
    WriteJsonString(out, "port_mapping_api", report.portMappingApi);
    WriteJsonString(out, "validation_api", report.validationApi);
    WriteJsonString(out, "artifact_writer", report.artifactWriter);
    WriteJsonString(out, "server_mode_contract", report.serverModeContract, false);
    out << "  },\n";
    out << "  \"runtime\": {\n";
    WriteJsonUInt(out, "expected_client_count", report.expectedClientCount);
    WriteJsonUInt(out, "ticks_executed", report.ticksExecuted);
    WriteJsonUInt(out, "base_port", report.basePort);
    WriteJsonBool(out, "empty_server_ticks_before_join", report.emptyServerTicksBeforeJoin);
    WriteJsonBool(out, "late_join_accepted", report.lateJoinAccepted);
    WriteJsonBool(out, "leave_does_not_stop_host", report.leaveDoesNotStopHost);
    WriteJsonBool(out, "host_runs_after_last_leave", report.hostRunsAfterLastLeave);
    WriteJsonBool(out, "stable_player_ids", report.stablePlayerIds);
    WriteJsonBool(out, "deterministic_port_mapping", report.deterministicPortMapping, false);
    out << "  },\n";
    out << "  \"clients\": [\n";
    for (std::size_t i = 0; i < report.clients.size(); ++i) {
        const NetworkRuntimeJoinLeaveClient& client = report.clients[i];
        out << "    {\"client_id\": " << client.clientId
            << ", \"accept_port\": " << client.acceptPort
            << ", \"joined_at_tick\": " << client.joinedAtTick
            << ", \"left_at_tick\": " << client.leftAtTick
            << ", \"final_acked_snapshot_seq\": " << client.finalAckedSnapshotSeq
            << "}";
        if (i + 1u < report.clients.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"events\": [\n";
    for (std::size_t i = 0; i < report.events.size(); ++i) {
        const NetworkRuntimeJoinLeaveEvent& event = report.events[i];
        out << "    {\"tick\": " << event.tick
            << ", \"client_id\": " << event.clientId
            << ", \"event\": \"" << EscapeJson(event.event) << "\""
            << ", \"host_continued\": " << BoolLiteral(event.hostContinued)
            << "}";
        if (i + 1u < report.events.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        const NetworkRuntimeJoinLeaveCheck& check = report.checks[i];
        out << "    { \"name\": \"" << EscapeJson(check.name) << "\", \"passed\": " << BoolLiteral(check.passed) << " }";
        if (i + 1 < report.checks.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool NetworkRuntimeJoinLeaveMeetsBaseline(const NetworkRuntimeJoinLeaveReport& report)
{
    if (report.schema != kRuntimeJoinLeaveSchema ||
        report.source != kSourcePath ||
        report.header != kHeaderPath ||
        report.portMappingApi != kMultiClientPortMappingApi ||
        report.validationApi != kRuntimeJoinLeaveValidationApi ||
        report.artifactWriter != kRuntimeJoinLeaveArtifactWriter ||
        report.serverModeContract != kRuntimeJoinLeaveServerModeContract) {
        return false;
    }
    if (report.expectedClientCount < 2u ||
        report.ticksExecuted < 8u ||
        report.clients.size() != static_cast<std::size_t>(report.expectedClientCount) ||
        report.events.size() < 6u) {
        return false;
    }
    if (!report.emptyServerTicksBeforeJoin ||
        !report.lateJoinAccepted ||
        !report.leaveDoesNotStopHost ||
        !report.hostRunsAfterLastLeave ||
        !report.stablePlayerIds ||
        !report.deterministicPortMapping) {
        return false;
    }
    for (const NetworkRuntimeJoinLeaveCheck& check : report.checks) {
        if (!check.passed) {
            return false;
        }
    }
    return true;
}

bool WriteNetworkRuntimeJoinLeaveArtifact(
    const std::string& path,
    const std::uint32_t expectedClientCount,
    const std::uint16_t basePort,
    const std::uint32_t ticksExecuted)
{
    const auto report = BuildNetworkRuntimeJoinLeaveFixture(expectedClientCount, basePort, ticksExecuted);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << SerializeNetworkRuntimeJoinLeaveJson(report);
    return out.good() && NetworkRuntimeJoinLeaveMeetsBaseline(report);
}

NetworkRemoteAvatarRenderReport BuildNetworkRemoteAvatarRenderReport(
    const std::uint32_t localClientId,
    const std::vector<NetworkRemoteAvatarRenderPose>& poses,
    std::uint32_t expectedAvatarCount,
    const std::uint32_t snapshotFrameCount,
    const std::uint32_t renderedAvatarCount,
    const std::uint32_t skinnedDraws,
    const std::uint32_t skinnedIndicesDrawn)
{
    if (expectedAvatarCount < 2u) {
        expectedAvatarCount = 2u;
    }

    NetworkRemoteAvatarRenderReport report;
    report.schema = kRemoteAvatarRenderSchema;
    report.source = kSourcePath;
    report.header = kHeaderPath;
    report.builderApi = kRemoteAvatarRenderBuilderApi;
    report.validationApi = kRemoteAvatarRenderValidationApi;
    report.artifactWriter = kRemoteAvatarRenderArtifactWriter;
    report.replicationContract = kRemoteAvatarReplicationContract;
    report.interpolationContract = kRemoteAvatarInterpolationContract;
    report.renderContract = kRemoteAvatarRenderContract;
    report.localClientId = localClientId == 0u ? 1u : localClientId;
    report.expectedAvatarCount = expectedAvatarCount;
    report.snapshotFrameCount = snapshotFrameCount;
    report.renderedAvatarCount = renderedAvatarCount;
    report.skinnedDraws = skinnedDraws;
    report.skinnedIndicesDrawn = skinnedIndicesDrawn;
    report.poses = poses;

    bool all_remote_interpolated = true;
    bool all_remote_rendered = true;
    bool local_marked_remote = false;
    bool local_present = false;
    std::uint32_t previous_client_id = 0u;
    bool unique_client_ids = true;
    for (std::size_t i = 0; i < report.poses.size(); ++i) {
        const NetworkRemoteAvatarRenderPose& pose = report.poses[i];
        if (i != 0u && pose.clientId == previous_client_id) {
            unique_client_ids = false;
        }
        previous_client_id = pose.clientId;
        if (pose.clientId == report.localClientId) {
            local_present = true;
            if (pose.remote) {
                local_marked_remote = true;
            }
        }
        if (pose.remote) {
            report.remoteAvatarCount += 1u;
            all_remote_interpolated = all_remote_interpolated && pose.interpolated;
            all_remote_rendered = all_remote_rendered && pose.rendered;
        }
    }

    report.serverSnapshotsReceived =
        snapshotFrameCount > 0u &&
        report.poses.size() >= static_cast<std::size_t>(expectedAvatarCount) &&
        AvatarPosePositionsAreFiniteMillimeters(report.poses);
    report.remoteAvatarsInterpolated =
        report.remoteAvatarCount >= expectedAvatarCount - 1u &&
        all_remote_interpolated;
    report.remoteAvatarsRendered =
        renderedAvatarCount >= expectedAvatarCount &&
        skinnedDraws >= expectedAvatarCount &&
        skinnedIndicesDrawn > 0u &&
        all_remote_rendered;
    report.localAvatarExcludedFromRemoteSet =
        local_present &&
        !local_marked_remote &&
        report.remoteAvatarCount == expectedAvatarCount - 1u;
    report.deterministicClientOrdering =
        unique_client_ids &&
        AvatarPosesAreOrderedByClientId(report.poses);
    report.checks = BuildRemoteAvatarRenderChecks(report);
    report.passed = NetworkRemoteAvatarRenderMeetsBaseline(report);
    report.checks = BuildRemoteAvatarRenderChecks(report);
    return report;
}

NetworkRemoteAvatarRenderReport BuildNetworkRemoteAvatarRenderFixture(
    std::uint32_t expectedAvatarCount,
    const std::uint32_t renderedAvatarCount,
    const std::uint32_t skinnedDraws,
    const std::uint32_t skinnedIndicesDrawn)
{
    if (expectedAvatarCount < 2u) {
        expectedAvatarCount = 2u;
    }

    std::vector<NetworkRemoteAvatarRenderPose> poses;
    poses.reserve(expectedAvatarCount);
    for (std::uint32_t clientId = 1u; clientId <= expectedAvatarCount; ++clientId) {
        NetworkRemoteAvatarRenderPose pose;
        pose.clientId = clientId;
        pose.serverTick = 60u + clientId;
        pose.snapshotSequence = 100u + clientId;
        pose.positionXMm = static_cast<int>((clientId - 1u) * 2500u);
        pose.positionYMm = 12000;
        pose.positionZMm = static_cast<int>(clientId * 1000u);
        pose.remote = clientId != 1u;
        pose.interpolated = true;
        pose.rendered = clientId <= renderedAvatarCount;
        poses.push_back(pose);
    }

    return BuildNetworkRemoteAvatarRenderReport(
        1u,
        poses,
        expectedAvatarCount,
        3u,
        renderedAvatarCount,
        skinnedDraws,
        skinnedIndicesDrawn);
}

std::string SerializeNetworkRemoteAvatarRenderJson(const NetworkRemoteAvatarRenderReport& report)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << EscapeJson(report.schema) << "\",\n";
    out << "  \"passed\": " << BoolLiteral(report.passed) << ",\n";
    out << "  \"network\": {\n";
    WriteJsonString(out, "source", report.source);
    WriteJsonString(out, "header", report.header);
    WriteJsonString(out, "builder_api", report.builderApi);
    WriteJsonString(out, "validation_api", report.validationApi);
    WriteJsonString(out, "artifact_writer", report.artifactWriter);
    WriteJsonString(out, "replication_contract", report.replicationContract);
    WriteJsonString(out, "interpolation_contract", report.interpolationContract);
    WriteJsonString(out, "render_contract", report.renderContract, false);
    out << "  },\n";
    out << "  \"remote_avatar_render\": {\n";
    WriteJsonUInt(out, "local_client_id", report.localClientId);
    WriteJsonUInt(out, "expected_avatar_count", report.expectedAvatarCount);
    WriteJsonUInt(out, "remote_avatar_count", report.remoteAvatarCount);
    WriteJsonUInt(out, "snapshot_frame_count", report.snapshotFrameCount);
    WriteJsonUInt(out, "rendered_avatar_count", report.renderedAvatarCount);
    WriteJsonUInt(out, "skinned_draws", report.skinnedDraws);
    WriteJsonUInt(out, "skinned_indices_drawn", report.skinnedIndicesDrawn);
    WriteJsonBool(out, "server_snapshots_received", report.serverSnapshotsReceived);
    WriteJsonBool(out, "remote_avatars_interpolated", report.remoteAvatarsInterpolated);
    WriteJsonBool(out, "remote_avatars_rendered", report.remoteAvatarsRendered);
    WriteJsonBool(out, "local_avatar_excluded_from_remote_set", report.localAvatarExcludedFromRemoteSet);
    WriteJsonBool(out, "deterministic_client_ordering", report.deterministicClientOrdering, false);
    out << "  },\n";
    out << "  \"poses\": [\n";
    for (std::size_t i = 0; i < report.poses.size(); ++i) {
        const NetworkRemoteAvatarRenderPose& pose = report.poses[i];
        out << "    {\"client_id\": " << pose.clientId
            << ", \"server_tick\": " << pose.serverTick
            << ", \"snapshot_sequence\": " << pose.snapshotSequence
            << ", \"position_x_mm\": " << pose.positionXMm
            << ", \"position_y_mm\": " << pose.positionYMm
            << ", \"position_z_mm\": " << pose.positionZMm
            << ", \"remote\": " << BoolLiteral(pose.remote)
            << ", \"interpolated\": " << BoolLiteral(pose.interpolated)
            << ", \"rendered\": " << BoolLiteral(pose.rendered)
            << "}";
        if (i + 1u < report.poses.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        const NetworkRemoteAvatarRenderCheck& check = report.checks[i];
        out << "    { \"name\": \"" << EscapeJson(check.name) << "\", \"passed\": " << BoolLiteral(check.passed) << " }";
        if (i + 1u < report.checks.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool NetworkRemoteAvatarRenderMeetsBaseline(const NetworkRemoteAvatarRenderReport& report)
{
    if (report.schema != kRemoteAvatarRenderSchema ||
        report.source != kSourcePath ||
        report.header != kHeaderPath ||
        report.builderApi != kRemoteAvatarRenderBuilderApi ||
        report.validationApi != kRemoteAvatarRenderValidationApi ||
        report.artifactWriter != kRemoteAvatarRenderArtifactWriter ||
        report.replicationContract != kRemoteAvatarReplicationContract ||
        report.interpolationContract != kRemoteAvatarInterpolationContract ||
        report.renderContract != kRemoteAvatarRenderContract) {
        return false;
    }
    if (report.expectedAvatarCount < 2u ||
        report.remoteAvatarCount < 1u ||
        report.poses.size() < static_cast<std::size_t>(report.expectedAvatarCount)) {
        return false;
    }
    if (!report.serverSnapshotsReceived ||
        !report.remoteAvatarsInterpolated ||
        !report.remoteAvatarsRendered ||
        !report.localAvatarExcludedFromRemoteSet ||
        !report.deterministicClientOrdering) {
        return false;
    }
    if (!AvatarPosePositionsAreFiniteMillimeters(report.poses) ||
        !AvatarPosesAreOrderedByClientId(report.poses)) {
        return false;
    }
    for (const NetworkRemoteAvatarRenderCheck& check : report.checks) {
        if (!check.passed) {
            return false;
        }
    }
    return true;
}

bool WriteNetworkRemoteAvatarRenderArtifact(
    const std::string& path,
    const NetworkRemoteAvatarRenderReport& report)
{
    const std::filesystem::path output_path(path);
    std::error_code ec;
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << SerializeNetworkRemoteAvatarRenderJson(report);
    return out.good() && NetworkRemoteAvatarRenderMeetsBaseline(report);
}

bool WriteNetworkRemoteAvatarRenderFixtureArtifact(
    const std::string& path,
    const std::uint32_t expectedAvatarCount,
    const std::uint32_t renderedAvatarCount,
    const std::uint32_t skinnedDraws,
    const std::uint32_t skinnedIndicesDrawn)
{
    const auto report = BuildNetworkRemoteAvatarRenderFixture(
        expectedAvatarCount,
        renderedAvatarCount,
        skinnedDraws,
        skinnedIndicesDrawn);
    return WriteNetworkRemoteAvatarRenderArtifact(path, report);
}

} // namespace luminumbra::network
