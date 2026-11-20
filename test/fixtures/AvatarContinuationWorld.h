#pragma once

#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/PlayerAvatar.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Luminumbra::TestSupport {

// Exercise continuation through the session bridge, including its owner checks.
// Jobs outlive the session, and all world creation happens before fault injection.
class AvatarContinuationWorld {
public:
    explicit AvatarContinuationWorld(bool started = true) {
        if (!started) {
            return;
        }
        jobs.startup(1);
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        session.SetActiveRegionsEnabled(false);
        if (!session.CreateTransientWorld("Avatar continuation", "12345", "default")) {
            throw std::runtime_error(session.GetWorldOpenError());
        }
    }

    Systems::PhysicsSystem& physics() {
        return *session.GetPhysicsSystem();
    }

    bool capture_player_owner(World::PhysicsOwnerState& output) {
        return session.CapturePlayerOwner(output);
    }

    bool restore_player_owner(const World::PhysicsOwnerState& state) {
        return session.RestorePlayerOwner(state);
    }

    bool capture_avatar_owner(std::size_t index, World::PhysicsOwnerState& output) {
        if (!session.GetPhysicsSystem()) {
            return false;
        }
        World::PlayerAvatar avatar;
        avatar.player_id = static_cast<std::uint32_t>(index);
        avatar.position = physics().get_avatar_position(index);
        avatar.velocity = physics().get_avatar_velocity(index);
        World::PlayerAvatarOwnerState captured;
        if (!session.CaptureAvatarOwner(avatar, index, captured)) {
            return false;
        }
        output = captured.physics;
        return true;
    }

    bool restore_avatar_owner(const World::PhysicsOwnerState& state,
                              std::size_t& index,
                              std::span<const World::PhysicsOwnerState> remaining_frozen = {}) {
        if (state.size < 136 || !World::ValidatePhysicsOwner(state)) {
            return false;
        }
        // The established scalar schema places feet at byte 84 and velocity at
        // byte 124. Validation above checks the entire payload before reading.
        const auto word = [&state](std::size_t offset) {
            std::uint32_t result = 0;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                result |= static_cast<std::uint32_t>(state.bytes[offset++]) << shift;
            }
            return result;
        };
        World::PlayerAvatarOwnerState captured;
        captured.physics = state;
        if (session.GetPhysicsSystem()) {
            captured.avatar.player_id =
                static_cast<std::uint32_t>(physics().avatar_character_slot_count());
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto wide = static_cast<std::uint64_t>(word(84 + 8 * axis)) |
                              (static_cast<std::uint64_t>(word(88 + 8 * axis)) << 32);
            captured.avatar.position[static_cast<int>(axis)] =
                static_cast<float>(std::bit_cast<double>(wide));
            captured.avatar.velocity[static_cast<int>(axis)] =
                std::bit_cast<float>(word(124 + 4 * axis));
        }
        World::PlayerAvatar output;
        return session.RestoreAvatarOwner(captured, output, index, remaining_frozen);
    }

private:
    // The root and jobs must outlive the session.
    class Root {
    public:
        Root()
            : directory(CreateUniqueTempDirectory("avatar_continuation_")) {
            // CMake compiles test sources by absolute path on every platform.
            const auto source = std::filesystem::path(__FILE__)
                                    .lexically_normal()
                                    .parent_path()
                                    .parent_path()
                                    .parent_path();
            std::filesystem::create_directories(directory / "worlds/atlas/presets");
            std::filesystem::copy_file(source / "worlds/atlas/presets/default.json",
                                       directory / "worlds/atlas/presets/default.json");
            CopyWorldgenTestData(source, directory);
        }
        ~Root() {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
        std::string root_string() const {
            return directory.string() +
                   static_cast<char>(std::filesystem::path::preferred_separator);
        }

    private:
        std::filesystem::path directory;
    };
    Root root;
    JobSystem jobs;
    world::GameSession session;
};

} // namespace Luminumbra::TestSupport
