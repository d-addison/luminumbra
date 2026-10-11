# Optional CPU-only adapter. Parent must provide the real prefab runtime target.
get_filename_component(STATIC_SCENE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(luminumbra_static_scene STATIC
    "${STATIC_SCENE_SOURCE_ROOT}/src/luminumbra_client/rendering/static_scene/StaticScene.cpp")
target_compile_features(luminumbra_static_scene PUBLIC cxx_std_20)
target_include_directories(luminumbra_static_scene PUBLIC
    "$<BUILD_INTERFACE:${STATIC_SCENE_SOURCE_ROOT}/include>"
    "$<INSTALL_INTERFACE:include>")
target_link_libraries(luminumbra_static_scene PRIVATE luminumbra_prefab_runtime)
install(TARGETS luminumbra_static_scene luminumbra_prefab_runtime ARCHIVE DESTINATION lib COMPONENT StaticScene)
install(FILES "${STATIC_SCENE_SOURCE_ROOT}/include/luminumbra/rendering/StaticScene.h"
    DESTINATION include/luminumbra/rendering COMPONENT StaticScene)
if(BUILD_TESTING)
    add_executable(static_scene_test "${STATIC_SCENE_SOURCE_ROOT}/test/static_scene/static_scene_test.cpp")
    target_link_libraries(static_scene_test PRIVATE luminumbra_static_scene luminumbra_prefab_runtime gtest_main)
    include(GoogleTest)
    gtest_discover_tests(static_scene_test PROPERTIES LABELS "authoring;rendering" TIMEOUT 30)
endif()
if(COMMAND luminumbra_configure_first_party_target)
    luminumbra_configure_first_party_target(luminumbra_static_scene)
    if(BUILD_TESTING)
        luminumbra_configure_first_party_target(static_scene_test)
    endif()
endif()
