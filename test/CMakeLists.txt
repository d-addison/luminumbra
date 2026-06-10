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

target_compile_definitions(sdf_gpu_cpu_parity_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(sdf_gpu_cpu_parity_test PRIVATE
    ../src
)

target_link_libraries(sdf_gpu_cpu_parity_test PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(worldgen_layer_snapshot_test
    shield/test_worldgen_layer_snapshots.cpp
)

target_compile_definitions(worldgen_layer_snapshot_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(worldgen_layer_snapshot_test PRIVATE
    ../src
)

target_link_libraries(worldgen_layer_snapshot_test PRIVATE
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
    common/EngineContracts_test.cpp
    common/JobSystem_test.cpp
    common/WorldStreamingState_test.cpp
)

target_compile_definitions(common_tests PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(common_tests PRIVATE
    ../src
)

target_link_libraries(common_tests PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(render_smoke_test
    rendering/render_smoke_test.cpp
)

target_compile_definitions(render_smoke_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(render_smoke_test PRIVATE
    ../src
)

target_link_libraries(render_smoke_test PRIVATE
    gtest_main
    glfw
    glad
)

if(WIN32)
    target_link_libraries(render_smoke_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_smoke_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_smoke_test PRIVATE OpenGL::GL)
endif()

add_executable(render_capture_test
    rendering/render_capture_test.cpp
)

target_compile_definitions(render_capture_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(render_capture_test PRIVATE
    ../src
)

target_link_libraries(render_capture_test PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(render_capture_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_capture_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(render_capture_test PRIVATE OpenGL::GL)
endif()

add_executable(ui_smoke_test
    ui/ui_smoke_test.cpp
    ../src/luminumbra_client/ui/core/UIStateManager.cpp
)

target_compile_definitions(ui_smoke_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(ui_smoke_test PRIVATE
    ../src
    ../src/luminumbra_client
)

target_link_libraries(ui_smoke_test PRIVATE
    gtest_main
    luminumbra_client
)

if(WIN32)
    target_link_libraries(ui_smoke_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(ui_smoke_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(ui_smoke_test PRIVATE OpenGL::GL)
endif()

add_executable(initial_world_loading_perf_test
    performance/initial_world_loading_perf_test.cpp
)

target_compile_definitions(initial_world_loading_perf_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(initial_world_loading_perf_test PRIVATE
    ../src
)

target_link_libraries(initial_world_loading_perf_test PRIVATE
    gtest_main
    luminumbra_common
)

add_executable(runtime_world_visual_validation_test
    performance/runtime_world_visual_validation_test.cpp
)

target_compile_definitions(runtime_world_visual_validation_test PRIVATE
    LUMINUMBRA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    LUMINUMBRA_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
)

target_include_directories(runtime_world_visual_validation_test PRIVATE
    ../src
)

target_link_libraries(runtime_world_visual_validation_test PRIVATE
    gtest_main
    luminumbra_common
    glfw
    glad
)

if(WIN32)
    target_link_libraries(runtime_world_visual_validation_test PRIVATE opengl32)
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(runtime_world_visual_validation_test PRIVATE ${OPENGL_LIBRARIES})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(runtime_world_visual_validation_test PRIVATE OpenGL::GL)
endif()

# Discover and add the tests to CTest for running
include(GoogleTest)
gtest_discover_tests(world_generation_test)
gtest_discover_tests(sdf_gpu_cpu_parity_test)
gtest_discover_tests(worldgen_layer_snapshot_test)
gtest_discover_tests(asset_processor_round_trip_test)
gtest_discover_tests(common_tests)
gtest_discover_tests(render_smoke_test)
gtest_discover_tests(render_capture_test)
gtest_discover_tests(ui_smoke_test)
gtest_discover_tests(initial_world_loading_perf_test)
gtest_discover_tests(runtime_world_visual_validation_test)

# The hidden GLFW runtime boot harness is kept as a manual tool in
# test/performance/run_runtime_boot_metrics.ps1. It is intentionally not a
# required CTest gate because hidden-window event handling is driver-dependent
# on Windows and can exit inside the renderer without diagnostic output.
