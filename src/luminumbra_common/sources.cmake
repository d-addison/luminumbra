# This file lists all source files for the 'luminumbra_common' target.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.
set(COMMON_SOURCES
    # AI
    ${CMAKE_CURRENT_LIST_DIR}/ai/InstinctPlanner.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ai/InstinctSystem.cpp

    # Fields
    ${CMAKE_CURRENT_LIST_DIR}/fields/ScalarFieldDiffusion.cpp

    # Animation
    ${CMAKE_CURRENT_LIST_DIR}/animation/AnimationRuntime.cpp

    # Core
    ${CMAKE_CURRENT_LIST_DIR}/common_placeholder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EngineContracts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EngineVersion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EventBus.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/JobSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/Log.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/SimulationClock.cpp

    # Net
    ${CMAKE_CURRENT_LIST_DIR}/net/NetworkManager.cpp
    # T-I4-13: delay-based lockstep transport (engine-generic; LoopbackTransport
    # for gates/tests, _WIN32-guarded TcpTransport for loopback+LAN).
    ${CMAKE_CURRENT_LIST_DIR}/net/LockstepSession.cpp

    # Network
    ${CMAKE_CURRENT_LIST_DIR}/network/NetworkLoopbackAuthority.cpp
    ${CMAKE_CURRENT_LIST_DIR}/network/NetworkStateHash.cpp

    # Persistence
    ${CMAKE_CURRENT_LIST_DIR}/persistence/WorldPersistenceRoundtrip.cpp
    ${CMAKE_CURRENT_LIST_DIR}/persistence/WorldSaveService.cpp

    # Replay (T-I4-12: LREC1 session replay stream; engine-generic)
    ${CMAKE_CURRENT_LIST_DIR}/replay/ReplayStream.cpp

    # Scripting
    ${CMAKE_CURRENT_LIST_DIR}/scripting/LuaApiManifest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scripting/LuaState.cpp

    # Simulation
    ${CMAKE_CURRENT_LIST_DIR}/simulation/SimulationEventBus.cpp

    # Shield
    ${CMAKE_CURRENT_LIST_DIR}/shield/SHIELDEngine.cpp

    # Systems
    ${CMAKE_CURRENT_LIST_DIR}/systems/PhysicsSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systems/SHIELD_WorldSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systems/WaterSystem.cpp
    # T-I5a-2 (A2): deterministic wind grid (sim-authoritative; world_hash wind slot).
    ${CMAKE_CURRENT_LIST_DIR}/systems/WindFieldSystem.cpp
    # T-I5a-3 (B1): deterministic weather core (sim-authoritative; world_hash weather slot).
    ${CMAKE_CURRENT_LIST_DIR}/systems/WeatherSystem.cpp

    # World
    ${CMAKE_CURRENT_LIST_DIR}/world/BiomeTable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/Chunk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/FarLodStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/GameSession.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/MarchingCubes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/StructurePlacement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/TerrainPresetLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/WorldStreamingState.cpp
)

# G1 pose-determinism gate (T-I3-15): forbid FP contraction in the animation
# runtime so debug and release sample bit-identical poses. GCC enables
# -ffp-contract=fast at -O2 by default; pinning it off here keeps the
# committed pose checksum preset-independent.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/animation/AnimationRuntime.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
