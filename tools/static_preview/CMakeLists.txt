get_filename_component(STATIC_PREVIEW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(dependency IN ITEMS luminumbra_prefab_runtime luminumbra_static_scene glad glfw spdlog)
    set_property(TARGET ${dependency} PROPERTY POSITION_INDEPENDENT_CODE ON)
endforeach()
# This independent image compiles the exact production units below. The game
# image compiles those same files through its existing source manifest; neither
# image links two copies of the units or the full game/client library.
add_library(luminumbra_render_static SHARED
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/static_scene/StaticScene.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/static_preview/RenderView.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/static_preview/StaticRenderer.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/passes/StaticDrawPass.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/passes/LightingPass.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/GBufferTargets.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/Shader.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/ShaderReflection.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/PassShaderLayouts.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/RenderResourceRegistry.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/core/Log.cpp")
target_compile_features(luminumbra_render_static PUBLIC cxx_std_20)
target_include_directories(luminumbra_render_static PUBLIC
    "$<BUILD_INTERFACE:${STATIC_PREVIEW_ROOT}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE "${STATIC_PREVIEW_ROOT}/src/luminumbra_client" "${STATIC_PREVIEW_ROOT}/src"
            "${STATIC_PREVIEW_ROOT}/src/luminumbra_common")
target_link_libraries(luminumbra_render_static PRIVATE luminumbra_prefab_runtime
    glad glfw spdlog glm ${CMAKE_DL_LIBS})
set_target_properties(luminumbra_render_static PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON
    EXPORT_NAME StaticPreview)
if(WIN32)
    target_link_libraries(luminumbra_render_static PRIVATE opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(luminumbra_render_static PRIVATE OpenGL::GL)
endif()
install(TARGETS luminumbra_render_static EXPORT LuminumbraStaticPreviewTargets
    LIBRARY DESTINATION lib COMPONENT StaticPreview
    RUNTIME DESTINATION bin COMPONENT StaticPreview
    ARCHIVE DESTINATION lib COMPONENT StaticPreview)
install(EXPORT LuminumbraStaticPreviewTargets NAMESPACE Luminumbra::
    DESTINATION lib/cmake/LuminumbraStaticPreview COMPONENT StaticPreview)
include(CMakePackageConfigHelpers)
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/LuminumbraStaticPreviewConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LuminumbraStaticPreviewConfig.cmake"
    INSTALL_DESTINATION lib/cmake/LuminumbraStaticPreview)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LuminumbraStaticPreviewConfig.cmake"
    DESTINATION lib/cmake/LuminumbraStaticPreview COMPONENT StaticPreview)
install(FILES "${STATIC_PREVIEW_ROOT}/include/luminumbra/rendering/RenderView.h"
              "${STATIC_PREVIEW_ROOT}/include/luminumbra/rendering/StaticRenderer.h"
              "${STATIC_PREVIEW_ROOT}/include/luminumbra/rendering/StaticScene.h"
    DESTINATION include/luminumbra/rendering COMPONENT StaticPreview)
install(FILES "${STATIC_PREVIEW_ROOT}/res/shaders/static_asset.vert"
              "${STATIC_PREVIEW_ROOT}/res/shaders/static_asset.frag"
              "${STATIC_PREVIEW_ROOT}/res/shaders/lighting_pass.vert"
              "${STATIC_PREVIEW_ROOT}/res/shaders/lighting_pass.frag"
              "${STATIC_PREVIEW_ROOT}/res/shaders/config_constants.gen.glsl"
    DESTINATION share/luminumbra-static/res/shaders COMPONENT StaticPreview)
if(BUILD_TESTING AND NOT CMAKE_SOURCE_DIR STREQUAL STATIC_PREVIEW_ROOT)
    add_executable(render_view_test "${STATIC_PREVIEW_ROOT}/test/rendering/render_view_test.cpp")
    target_link_libraries(render_view_test PRIVATE luminumbra_render_static gtest_main glm)
    include(GoogleTest)
    if(NOT CMAKE_SOURCE_DIR STREQUAL STATIC_PREVIEW_ROOT)
        gtest_discover_tests(render_view_test PROPERTIES LABELS "rendering;authoring" TIMEOUT 30)
    endif()
endif()
if(COMMAND luminumbra_configure_first_party_target)
    luminumbra_configure_first_party_target(luminumbra_render_static)
    if(TARGET render_view_test)
        luminumbra_configure_first_party_target(render_view_test)
    endif()
endif()

# Exact source-content identity survives local dirty development builds; the
# separate Git dirty bit prevents presenting that build as its clean parent.
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${STATIC_PREVIEW_ROOT}"
    OUTPUT_VARIABLE PREVIEW_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${STATIC_PREVIEW_ROOT}"
    OUTPUT_VARIABLE PREVIEW_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(PREVIEW_DIRTY false)
if(NOT PREVIEW_STATUS STREQUAL "")
    set(PREVIEW_DIRTY true)
endif()
get_target_property(PREVIEW_SOURCES luminumbra_render_static SOURCES)
file(GLOB_RECURSE PREVIEW_IDENTITY_INPUTS CONFIGURE_DEPENDS
    "${STATIC_PREVIEW_ROOT}/include/luminumbra/rendering/*.h"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/authoring/*.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/authoring/*.h"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_client/rendering/*.h")
list(APPEND PREVIEW_IDENTITY_INPUTS ${PREVIEW_SOURCES} "${CMAKE_CURRENT_LIST_FILE}"
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/LuminumbraStaticPreviewConfig.cmake.in"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/core/FilesystemPath.cpp"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/core/FilesystemPath.h"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/core/Log.h"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/core/Environment.h"
    "${STATIC_PREVIEW_ROOT}/src/luminumbra_common/components/CoreComponents.h"
    "${STATIC_PREVIEW_ROOT}/include/luminumbra/core/Types.h"
    "${STATIC_PREVIEW_ROOT}/tools/prefab_runtime/CMakeLists.txt"
    "${STATIC_PREVIEW_ROOT}/tools/static_scene/CMakeLists.txt")
foreach(shader IN ITEMS static_asset.vert static_asset.frag lighting_pass.vert lighting_pass.frag config_constants.gen.glsl)
    list(APPEND PREVIEW_IDENTITY_INPUTS "${STATIC_PREVIEW_ROOT}/res/shaders/${shader}")
endforeach()
list(SORT PREVIEW_IDENTITY_INPUTS)
list(REMOVE_DUPLICATES PREVIEW_IDENTITY_INPUTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PREVIEW_IDENTITY_INPUTS})
foreach(git_member IN ITEMS HEAD index logs/HEAD)
    execute_process(COMMAND git rev-parse --git-path "${git_member}"
        WORKING_DIRECTORY "${STATIC_PREVIEW_ROOT}" OUTPUT_VARIABLE git_path
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    get_filename_component(git_path "${git_path}" ABSOLUTE BASE_DIR "${STATIC_PREVIEW_ROOT}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_path}")
endforeach()
set(PREVIEW_HASH_INPUT "")
foreach(input IN LISTS PREVIEW_IDENTITY_INPUTS)
    file(SHA256 "${input}" digest)
    file(RELATIVE_PATH relative "${STATIC_PREVIEW_ROOT}" "${input}")
    string(APPEND PREVIEW_HASH_INPUT "${relative}:${digest}\n")
endforeach()
string(SHA256 PREVIEW_SOURCE_DIGEST "${PREVIEW_HASH_INPUT}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/source-inputs.txt" "${PREVIEW_HASH_INPUT}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/PreviewBuildIdentity.h"
    "#pragma once\ninline constexpr const char* kPreviewCommit = \"${PREVIEW_COMMIT}\";\ninline constexpr bool kPreviewDirty = ${PREVIEW_DIRTY};\ninline constexpr const char* kPreviewSourceDigest = \"${PREVIEW_SOURCE_DIGEST}\";\n")
add_executable(luminumbra_preview_capture main.cpp)
target_include_directories(luminumbra_preview_capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(luminumbra_preview_capture PRIVATE luminumbra_render_static luminumbra_prefab_runtime)
set_target_properties(luminumbra_preview_capture PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
install(TARGETS luminumbra_preview_capture RUNTIME DESTINATION bin COMPONENT StaticPreview)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/source-inputs.txt"
    DESTINATION share/luminumbra-static COMPONENT StaticPreview)
if(COMMAND luminumbra_configure_first_party_target)
    luminumbra_configure_first_party_target(luminumbra_preview_capture)
endif()

if(BUILD_TESTING)
    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
    if(CMAKE_GENERATOR MATCHES "Ninja")
        add_test(NAME StaticPreviewSourceIdentity COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/check_source_identity.py"
            --source "${STATIC_PREVIEW_ROOT}" --build "${CMAKE_BINARY_DIR}"
            --manifest "${CMAKE_CURRENT_BINARY_DIR}/source-inputs.txt"
            --ninja "${CMAKE_MAKE_PROGRAM}")
        set_tests_properties(StaticPreviewSourceIdentity PROPERTIES LABELS "authoring;tools" TIMEOUT 90)
    endif()
    if(NOT CMAKE_SOURCE_DIR STREQUAL STATIC_PREVIEW_ROOT)
        add_test(NAME StaticPreviewCaptureContract COMMAND "${Python3_EXECUTABLE}"
            "${STATIC_PREVIEW_ROOT}/test/static_preview/test_capture_validation.py")
        set_tests_properties(StaticPreviewCaptureContract PROPERTIES LABELS "authoring;tools" TIMEOUT 90)
    endif()
    add_executable(static_preview_render_test "${STATIC_PREVIEW_ROOT}/test/rendering/static_preview_render_test.cpp")
    target_link_libraries(static_preview_render_test PRIVATE luminumbra_render_static gtest_main glm glad entt)
    target_include_directories(static_preview_render_test PRIVATE
        "${STATIC_PREVIEW_ROOT}/src/luminumbra_client" "${STATIC_PREVIEW_ROOT}/src/luminumbra_common")
    target_compile_definitions(static_preview_render_test PRIVATE STATIC_PREVIEW_RESOURCE_ROOT="${STATIC_PREVIEW_ROOT}")
    if(COMMAND luminumbra_configure_first_party_target)
        luminumbra_configure_first_party_target(static_preview_render_test)
    endif()
    if(UNIX AND NOT APPLE)
        find_program(STATIC_PREVIEW_XVFB Xvfb REQUIRED)
        add_test(NAME StaticPreviewSoftwareGL
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_software_gl.py"
                --binary "$<TARGET_FILE:static_preview_render_test>" --xvfb "${STATIC_PREVIEW_XVFB}")
        set_tests_properties(StaticPreviewSoftwareGL PROPERTIES LABELS "rendering;authoring;software-gl" TIMEOUT 200)
    endif()
endif()
