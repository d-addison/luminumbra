# This file lists all source files for the 'luminumbra_common' target.
# Paths are anchored to this file so the list is safe to include from any
# CMakeLists.txt in the tree.
set(COMMON_SOURCES
    # Core
    ${CMAKE_CURRENT_LIST_DIR}/common_placeholder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EngineContracts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EngineVersion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/EventBus.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/JobSystem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/Log.cpp

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
    ${CMAKE_CURRENT_LIST_DIR}/world/Chunk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/GameSession.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world/MarchingCubes.cpp
)
