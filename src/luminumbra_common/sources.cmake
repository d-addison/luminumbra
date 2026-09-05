# This file lists all source files for the 'luminumbra_common' target.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.
set(COMMON_SOURCES
    # AI
    ${CMAKE_CURRENT_LIST_DIR}/ai/InstinctPlanner.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ai/InstinctSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ai/InstinctLocomotionSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ai/StimulusChannels.cpp

    # Fields
    ${CMAKE_CURRENT_LIST_DIR}/fields/ScalarFieldDiffusion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fields/EnergyFieldState.cpp

    # Animation
    ${CMAKE_CURRENT_LIST_DIR}/animation/AnimationRuntime.cpp

    # Core
    ${CMAKE_CURRENT_LIST_DIR}/core/EngineContracts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EngineVersion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EventBus.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/JobSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/Log.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/SimulationClock.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/SystemConfig.cpp

    # Net
    # delay-based lockstep transport (engine-generic; LoopbackTransport
    # for gates/tests, _WIN32-guarded TcpTransport for loopback+LAN).
    ${CMAKE_CURRENT_LIST_DIR}/net/LockstepSession.cpp
    #  authoritative-server state-replication wire protocol.
    ${CMAKE_CURRENT_LIST_DIR}/net/ReplicationProtocol.cpp
    #  server/client replication endpoints over ILockstepTransport.
    ${CMAKE_CURRENT_LIST_DIR}/net/ReplicationEndpoint.cpp
    # Steamworks ISteamNetworkingSockets transport (body guarded by
    # LUMINUMBRA_ENABLE_STEAM; compiles to nothing when the SDK is not wired in).
    ${CMAKE_CURRENT_LIST_DIR}/net/NetSocketsTransport.cpp
    ${CMAKE_CURRENT_LIST_DIR}/net/SteamNetworkingTransport.cpp
    # standalone GameNetworkingSockets transport (body guarded by
    # LUMINUMBRA_ENABLE_GNS; compiles to nothing when GNS is not wired in).
    ${CMAKE_CURRENT_LIST_DIR}/net/GnsTransport.cpp

    # Network
    ${CMAKE_CURRENT_LIST_DIR}/network/NetworkLoopbackAuthority.cpp
    ${CMAKE_CURRENT_LIST_DIR}/network/NetworkStateHash.cpp

    # Persistence
    ${CMAKE_CURRENT_LIST_DIR}/persistence/WorldPersistenceRoundtrip.cpp
    ${CMAKE_CURRENT_LIST_DIR}/persistence/WorldSaveService.cpp

    # Replay (: LREC1 session replay stream; engine-generic)
    ${CMAKE_CURRENT_LIST_DIR}/replay/ReplayStream.cpp

    # Scripting
    ${CMAKE_CURRENT_LIST_DIR}/scripting/LuaApiManifest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scripting/LuaState.cpp

    # Simulation
    ${CMAKE_CURRENT_LIST_DIR}/simulation/SimulationEventBus.cpp

    # Shield

    # Systems
    ${CMAKE_CURRENT_LIST_DIR}/systems/PhysicsSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systems/SHIELD_WorldSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systems/WaterSystem.cpp
    # deterministic wind grid (sim-authoritative; world_hash wind slot).
    ${CMAKE_CURRENT_LIST_DIR}/systems/WindFieldSystem.cpp
    # deterministic weather core (sim-authoritative; world_hash weather slot).
    ${CMAKE_CURRENT_LIST_DIR}/systems/WeatherSystem.cpp
    # deterministic Aether scalar field (sim-authoritative; world_hash aether slot).
    ${CMAKE_CURRENT_LIST_DIR}/systems/AetherFieldSystem.cpp
    # deterministic thermal+hydraulic erosion kernel (worldgen primitive).
    ${CMAKE_CURRENT_LIST_DIR}/world/HydraulicErosion.cpp

    # World
    ${CMAKE_CURRENT_LIST_DIR}/world/BiomeTable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/Chunk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/FarLodStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/GameSession.cpp
    #  semantic-knob -> generation_params response table + persisted layer.
    ${CMAKE_CURRENT_LIST_DIR}/world/KnobLayer.cpp
    #  constrained fixed-topology layer graph (compiles bit-exact to params).
    ${CMAKE_CURRENT_LIST_DIR}/world/LayerGraph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/MarchingCubes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/PlayerAvatar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/StructurePlacement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/TerrainPresetLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/WorldStreamingState.cpp
)

# Animation pose determinism is covered by the target-wide floating-point
# contract in the root build. Keeping the policy at target scope also prevents
# compiler-specific switches from leaking onto MSVC source invocations.
