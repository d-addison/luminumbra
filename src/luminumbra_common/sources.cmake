# This file lists all source files for the 'luminumbra_common' target.
# Paths are relative to the 'src/luminumbra_common' directory.
set(COMMON_SOURCES
    # Core
    core/EventBus.cpp
    core/JobSystem.cpp
    core/Log.cpp

    # Net
    net/NetworkManager.cpp

    # Scripting
    scripting/LuaState.cpp

    # Shield
    shield/SHIELDEngine.cpp

    # Systems
    systems/PhysicsSystem.cpp
    systems/SHIELD_WorldSystem.cpp

    # Vendor
    vendor/FastNoiseLite.cpp

    # World
    world/Chunk.cpp
    world/GameSession.cpp
    world/MarchingCubes.cpp
)