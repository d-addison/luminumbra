#include "luminumbra_common/network/NetworkLoopbackAuthority.h"

#include <string>

namespace luminumbra::network::test {

bool NetworkLoopbackAuthorityGateExercisesFixture() {
    const auto report = BuildNetworkLoopbackConvergenceFixture("debug");
    const std::string serialized = SerializeNetworkLoopbackConvergenceJson(report);
    return NetworkLoopbackAuthorityMeetsBaseline(report) &&
           serialized.find("server_authoritative_loopback_reconciliation") != std::string::npos &&
           serialized.find("client_authority_claim_rejected") != std::string::npos;
}

} // namespace luminumbra::network::test
