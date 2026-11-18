// Test-only link seam for the dormant GameSession authority profile.
//
// GameSession.h declares the AuthorityProfileTestOnly constructor but src/ never defines
// it. This file is its only definition and is listed only in test/CMakeLists.txt, so
// production binaries cannot link a non-default authority profile until slice I binds
// the option to negotiated identity.
#include "luminumbra_common/world/GameSession.h"

namespace Luminumbra::world {

GameSession::GameSession(const AuthorityProfileOptions& authority_profile,
                         AuthorityProfileTestOnly /*test_only*/)
    : GameSession(authority_profile, AuthorityProfileTag{}) {}

} // namespace Luminumbra::world
