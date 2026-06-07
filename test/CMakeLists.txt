# Add the world generation test executable
add_executable(world_generation_test
    shield/test_world_generation.cpp
)

target_include_directories(world_generation_test PRIVATE
    ../src
)

target_link_libraries(world_generation_test PRIVATE
    gtest_main
    luminumbra_common
)

# Discover and add the tests to CTest for running
include(GoogleTest)
gtest_discover_tests(world_generation_test)