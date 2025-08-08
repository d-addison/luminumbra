# Add the test executable
add_executable(world_generation_test
    shield/test_world_generation.cpp
    ../src/luminumbra_common/systems/SHIELD_WorldSystem.cpp
    ../src/luminumbra_common/world/Chunk.cpp
    ../src/luminumbra_common/core/JobSystem.cpp
    ../src/luminumbra_common/vendor/FastNoiseLite.cpp
)

# Link against GTest and other necessary libraries
target_link_libraries(world_generation_test
    gtest
    gtest_main
    luminumbra_common
    fastnoise
)

target_include_directories(world_generation_test PRIVATE
    ../src
    ../vendor/googletest/googletest/include
)

# Add the test to CTest
include(GoogleTest)
gtest_discover_tests(world_generation_test)
