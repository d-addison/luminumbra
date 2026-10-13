# tools/CMakeLists.txt

add_executable(asset_processor
    asset_processor.cpp
)

# Tell the target where to find "meshoptimizer.h"
target_include_directories(asset_processor PUBLIC
    # For luminumbra_common/animation/SkinnedMeshFormat.h (LMS2/.lanim formats)
    ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(asset_processor SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/cgltf
)
# Vendor headers are SYSTEM includes: third-party code (stb, meshoptimizer)
# is not held to the project's -Werror warning policy.
target_link_libraries(asset_processor PRIVATE meshoptimizer stb_headers)

option(LUMINUMBRA_ENABLE_PREFAB_RUNTIME "Build the optional compiled prefab ECS runtime and inspector" OFF)
option(LUMINUMBRA_ENABLE_STATIC_PREVIEW "Build the neutral production static renderer and installed capture host" OFF)
if(LUMINUMBRA_ENABLE_PREFAB_RUNTIME OR LUMINUMBRA_ENABLE_STATIC_PREVIEW)
    add_subdirectory(prefab_runtime)
endif()
if(LUMINUMBRA_ENABLE_STATIC_PREVIEW)
    add_subdirectory(static_scene)
    add_subdirectory(static_preview)
endif()

if(BUILD_TESTING)
    # Acceptance fixture code is linked only by its CLI and tests, never the engine targets.
    add_library(luminumbra_save_fixture_support STATIC
        ${CMAKE_SOURCE_DIR}/src/luminumbra_common/persistence/SyntheticSaveFixture.cpp)
    target_include_directories(luminumbra_save_fixture_support PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(luminumbra_save_fixture_support PUBLIC luminumbra_common)
    luminumbra_configure_first_party_target(luminumbra_save_fixture_support)

    add_executable(luminumbra_save_fixture persistence/save_fixture_main.cpp)
    target_link_libraries(luminumbra_save_fixture PRIVATE luminumbra_save_fixture_support)
    luminumbra_configure_first_party_target(luminumbra_save_fixture)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME PerformanceToolContract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_perf.py")
    set_tests_properties(PerformanceToolContract PROPERTIES LABELS "perf;tools" TIMEOUT 30)
    add_test(
        NAME PerformanceMeasurementValidatorContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_validate_performance_measurement.py")
    set_tests_properties(PerformanceMeasurementValidatorContract
        PROPERTIES LABELS "perf;tools" TIMEOUT 30)
    add_test(
        NAME RenderBudgetPython
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_render_budget.py")
    set_tests_properties(RenderBudgetPython PROPERTIES LABELS "perf;tools" TIMEOUT 30)
    add_test(
        NAME PublicTreeValidatorContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_validate_public_tree.py")
    set_tests_properties(PublicTreeValidatorContract
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME PublicAssetInventory
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/validate_assets.py"
                --root "${CMAKE_SOURCE_DIR}"
                --check "${CMAKE_SOURCE_DIR}/config/public-asset-inventory.json")
    set_tests_properties(PublicAssetInventory
        PROPERTIES LABELS "hygiene;assets;tools" TIMEOUT 60)
    add_test(
        NAME SaveReceiptContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/persistence/test_save_receipt.py")
    set_tests_properties(SaveReceiptContract
        PROPERTIES LABELS "persistence;tools" TIMEOUT 30)
    add_test(
        NAME SavedWorldAcceptanceReceiptContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/persistence/test_check_acceptance_receipt.py")
    set_tests_properties(SavedWorldAcceptanceReceiptContract
        PROPERTIES LABELS "persistence;tools" TIMEOUT 30)
    add_test(
        NAME V03AcceptanceReceiptContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_v03_receipt.py")
    set_tests_properties(V03AcceptanceReceiptContract
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME DocumentationLinkValidatorContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_docs.py")
    set_tests_properties(DocumentationLinkValidatorContract
        PROPERTIES LABELS "docs;hygiene;tools" TIMEOUT 30)
    add_test(
        NAME SlowestTestsScriptContract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_slowest_tests.py")
    set_tests_properties(SlowestTestsScriptContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
endif()
