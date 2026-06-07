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

add_executable(sdf_gpu_cpu_parity_test
    shield/test_sdf_gpu_cpu_parity.cpp
)

target_include_directories(sdf_gpu_cpu_parity_test PRIVATE
    ../src
)

target_link_libraries(sdf_gpu_cpu_parity_test PRIVATE
    gtest_main
    luminumbra_common
)

set(ASSET_PROCESSOR_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/asset_processor.cpp")
set_source_files_properties("${ASSET_PROCESSOR_SOURCE}" PROPERTIES
    COMPILE_DEFINITIONS "main=asset_processor_main"
)

add_executable(asset_processor_round_trip_test
    tools/test_asset_processor_round_trip.cpp
    "${ASSET_PROCESSOR_SOURCE}"
)

target_include_directories(asset_processor_round_trip_test PRIVATE
    ../src
    ../tools
    ../vendor/cgltf
    ../vendor/meshoptimizer/src
    ../third_party/cgltf
    ../third_party/meshoptimizer/src
    ../external/cgltf
    ../external/meshoptimizer/src
)

if(TARGET AssetProcessor)
    get_target_property(ASSET_PROCESSOR_INCLUDE_DIRS AssetProcessor INCLUDE_DIRECTORIES)
    if(ASSET_PROCESSOR_INCLUDE_DIRS)
        target_include_directories(asset_processor_round_trip_test PRIVATE
            ${ASSET_PROCESSOR_INCLUDE_DIRS}
        )
    endif()

    get_target_property(ASSET_PROCESSOR_LINK_LIBRARIES AssetProcessor LINK_LIBRARIES)
    if(ASSET_PROCESSOR_LINK_LIBRARIES)
        target_link_libraries(asset_processor_round_trip_test PRIVATE
            ${ASSET_PROCESSOR_LINK_LIBRARIES}
        )
    endif()
endif()

target_link_libraries(asset_processor_round_trip_test PRIVATE
    gtest_main
)

if(TARGET meshoptimizer)
    target_link_libraries(asset_processor_round_trip_test PRIVATE meshoptimizer)
elseif(TARGET meshoptimizer::meshoptimizer)
    target_link_libraries(asset_processor_round_trip_test PRIVATE meshoptimizer::meshoptimizer)
endif()

add_executable(common_tests
    common/JobSystem_test.cpp
    common/WorldStreamingState_test.cpp
)

target_include_directories(common_tests PRIVATE
    ../src
)

target_link_libraries(common_tests PRIVATE
    gtest_main
    luminumbra_common
)

# Discover and add the tests to CTest for running
include(GoogleTest)
gtest_discover_tests(world_generation_test)
gtest_discover_tests(sdf_gpu_cpu_parity_test)
gtest_discover_tests(asset_processor_round_trip_test)
gtest_discover_tests(common_tests)
