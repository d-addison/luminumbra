# This file lists all source files for the 'luminumbra_common' target.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.
set(COMMON_SOURCES
    # Core
<<<<<<< HEAD
    ${CMAKE_CURRENT_LIST_DIR}/common_placeholder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EventBus.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/JobSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/Log.cpp
=======
    core/EngineVersion.cpp
    core/EventBus.cpp
    core/JobSystem.cpp
    core/Log.cpp
>>>>>>> 536500a (T-OD4-embed-git-sha-engine-version: Embed git SHA in engine version)

    # Net
    ${CMAKE_CURRENT_LIST_DIR}/net/NetworkManager.cpp

    # Scripting
    ${CMAKE_CURRENT_LIST_DIR}/scripting/LuaState.cpp

    # Shield
    ${CMAKE_CURRENT_LIST_DIR}/shield/SHIELDEngine.cpp

    # Systems
    ${CMAKE_CURRENT_LIST_DIR}/systems/PhysicsSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systems/SHIELD_WorldSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systems/WaterSystem.cpp

    # World
<<<<<<< HEAD
    ${CMAKE_CURRENT_LIST_DIR}/world/Chunk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/GameSession.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/MarchingCubes.cpp
=======
    world/Chunk.cpp
    world/GameSession.cpp
    world/MarchingCubes.cpp
>>>>>>> 536500a (T-OD4-embed-git-sha-engine-version: Embed git SHA in engine version)
)
