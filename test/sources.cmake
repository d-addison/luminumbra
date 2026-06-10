# This file lists test source files that are shared by CTest executables.
# Paths are anchored to this file so it is safe to include from any test
# CMakeLists.txt in the tree.
set(SIMULATION_TEST_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/simulation/eventbus_order_gate_test.cpp
)

set(SCRIPTING_TEST_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/scripting/lua_api_manifest_gate_test.cpp
)

set(LUMINUMBRA_TEST_SOURCES
    ${LUMINUMBRA_TEST_SOURCES}
    ${SIMULATION_TEST_SOURCES}
    ${SCRIPTING_TEST_SOURCES}
)

set(TEST_SOURCES
    ${TEST_SOURCES}
    ${SIMULATION_TEST_SOURCES}
    ${SCRIPTING_TEST_SOURCES}
)
