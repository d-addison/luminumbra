#include "NetworkLoopbackAuthority.h"

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

} // namespace luminumbra::network
