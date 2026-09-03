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

if(BUILD_TESTING)
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
        NAME DocumentationLinkValidatorContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_docs.py")
    set_tests_properties(DocumentationLinkValidatorContract
        PROPERTIES LABELS "docs;hygiene;tools" TIMEOUT 30)
endif()
