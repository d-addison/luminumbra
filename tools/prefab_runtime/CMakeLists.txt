# Optional, GL-free runtime module and installed inspection consumer.
get_filename_component(PREFAB_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(luminumbra_prefab_runtime STATIC
    "${PREFAB_SOURCE_ROOT}/src/luminumbra_common/authoring/PrefabAsset.cpp"
    "${PREFAB_SOURCE_ROOT}/src/luminumbra_common/authoring/PrefabScene.cpp"
    "${PREFAB_SOURCE_ROOT}/src/luminumbra_common/authoring/PrefabDigest.cpp"
    "${PREFAB_SOURCE_ROOT}/src/luminumbra_common/core/FilesystemPath.cpp")
target_compile_features(luminumbra_prefab_runtime PUBLIC cxx_std_20)
target_include_directories(luminumbra_prefab_runtime PUBLIC
    "${PREFAB_SOURCE_ROOT}/src/luminumbra_common")
target_link_libraries(luminumbra_prefab_runtime PUBLIC entt glm nlohmann_json)
add_executable(luminumbra_prefab_inspect main.cpp)
target_link_libraries(luminumbra_prefab_inspect PRIVATE luminumbra_prefab_runtime)
install(TARGETS luminumbra_prefab_inspect RUNTIME DESTINATION bin COMPONENT PrefabRuntime)
if(BUILD_TESTING)
    add_executable(prefab_runtime_test "${PREFAB_SOURCE_ROOT}/test/authoring/prefab_runtime_test.cpp")
    target_link_libraries(prefab_runtime_test PRIVATE luminumbra_prefab_runtime gtest_main)
    include(GoogleTest)
    gtest_discover_tests(prefab_runtime_test PROPERTIES LABELS "authoring" TIMEOUT 30)
endif()
if(COMMAND luminumbra_configure_first_party_target)
    luminumbra_configure_first_party_target(luminumbra_prefab_runtime)
    luminumbra_configure_first_party_target(luminumbra_prefab_inspect)
    if(BUILD_TESTING)
        luminumbra_configure_first_party_target(prefab_runtime_test)
    endif()
endif()
