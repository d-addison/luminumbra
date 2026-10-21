# tools/CMakeLists.txt

add_executable(asset_processor
    asset_processor.cpp
    morph_asset_processor.cpp
    "${CMAKE_SOURCE_DIR}/src/luminumbra_common/animation/MorphMesh.cpp"
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

# CPU-only station discovery uses the common query surface without a server lifecycle.
add_executable(luminumbra_station_discovery station_discovery.cpp)
target_link_libraries(luminumbra_station_discovery PRIVATE luminumbra_common)
luminumbra_configure_first_party_target(luminumbra_station_discovery)
luminumbra_pin_fp_determinism(luminumbra_station_discovery)
if(MINGW)
    # Select the CRT startup that supplies UTF-16 argv to wmain.
    target_link_options(luminumbra_station_discovery PRIVATE -municode)
endif()
luminumbra_baseline_startup("${CMAKE_CURRENT_SOURCE_DIR}/station_discovery.cpp")
set(LUMINUMBRA_STATION_ENGINE_REVISION "" CACHE STRING "Pinned discovery source revision")
if(NOT LUMINUMBRA_STATION_ENGINE_REVISION)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
            OUTPUT_VARIABLE LUMINUMBRA_STATION_ENGINE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
endif()
target_compile_definitions(luminumbra_station_discovery PRIVATE
    LUMINUMBRA_STATION_ENGINE_REVISION="${LUMINUMBRA_STATION_ENGINE_REVISION}")

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
    # Compile the shipping entry separately so the harness changes the FP state
    # before calling it, without a production injection switch or header seam.
    add_library(station_discovery_numeric_entry OBJECT station_discovery.cpp)
    target_link_libraries(station_discovery_numeric_entry PRIVATE luminumbra_common)
    target_compile_definitions(station_discovery_numeric_entry PRIVATE
        main=StationDiscoveryEntry wmain=StationDiscoveryEntry
        LUMINUMBRA_STATION_ENGINE_REVISION="${LUMINUMBRA_STATION_ENGINE_REVISION}")
    luminumbra_configure_first_party_target(station_discovery_numeric_entry)
    luminumbra_pin_fp_determinism(station_discovery_numeric_entry)
    add_executable(station_discovery_numeric_environment
        "${CMAKE_SOURCE_DIR}/test/common/StationDiscoveryNumericEnvironment.cpp"
        $<TARGET_OBJECTS:station_discovery_numeric_entry>)
    target_link_libraries(station_discovery_numeric_environment PRIVATE luminumbra_common)
    luminumbra_configure_first_party_target(station_discovery_numeric_environment)
    luminumbra_pin_fp_determinism(station_discovery_numeric_environment)
    luminumbra_baseline_startup(
        "${CMAKE_SOURCE_DIR}/test/common/StationDiscoveryNumericEnvironment.cpp")
    if(MINGW)
        target_link_options(station_discovery_numeric_environment PRIVATE -municode)
    endif()
    add_test(
        NAME NetworkingImportedFilesContract
        COMMAND "${CMAKE_COMMAND}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_networking_imported_files.cmake")
    set_tests_properties(NetworkingImportedFilesContract
        PROPERTIES LABELS "ci;tools;net" TIMEOUT 30)

    add_test(
        NAME NetworkingRuntimeContract
        COMMAND "${CMAKE_COMMAND}"
                "-DNETWORK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/networking-runtime-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_networking_runtime.cmake")
    set_tests_properties(NetworkingRuntimeContract
        PROPERTIES LABELS "ci;tools;net" TIMEOUT 30)

    # The staging module needs CMake 3.21+ on Windows; 3.20 builds stay networking-free.
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
        add_test(
            NAME NetworkingRuntimeBuildIntegration
            COMMAND "${CMAKE_COMMAND}"
                    "-DNETWORK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/networking-runtime-integration"
                    "-DNETWORK_TEST_GENERATOR=${CMAKE_GENERATOR}"
                    "-DNETWORK_TEST_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                    "-DNETWORK_TEST_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                    "-DNETWORK_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                    "-DNETWORK_TEST_COMPILER=${CMAKE_CXX_COMPILER}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_networking_runtime_integration.cmake")
        set_tests_properties(NetworkingRuntimeBuildIntegration
            PROPERTIES LABELS "ci;tools;net" TIMEOUT 90)
    endif()

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
        NAME WorldgenProbeSmokeValidatorContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_worldgen_probe_smoke.py")
    set_tests_properties(WorldgenProbeSmokeValidatorContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    if(UNIX AND NOT WIN32)
        find_program(UCRT_ATTRIBUTION_BASH bash REQUIRED)
        add_test(
            NAME UcrtAttributionContract
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ucrt_attribution.py"
                    --bash "${UCRT_ATTRIBUTION_BASH}")
        set_tests_properties(UcrtAttributionContract
            PROPERTIES LABELS "ci;tools" TIMEOUT 30)
        add_test(
            NAME UcrtGccCohortContract
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ucrt_gcc_cohort.py"
                    --bash "${UCRT_ATTRIBUTION_BASH}")
        set_tests_properties(UcrtGccCohortContract
            PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    endif()
    add_test(
        NAME PerformanceToolContract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_perf.py")
    set_tests_properties(PerformanceToolContract PROPERTIES LABELS "perf;tools" TIMEOUT 30)
    add_test(
        NAME ResidencyCheckerPython
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_check_residency.py")
    set_tests_properties(ResidencyCheckerPython PROPERTIES LABELS "perf;tools" TIMEOUT 60)
    add_test(
        NAME QualificationPlanPython
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_plan_qualification.py")
    set_tests_properties(QualificationPlanPython
        PROPERTIES LABELS "perf;tools" TIMEOUT 60)
    add_test(
        NAME QualificationRunnerPython
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_run_qualification.py")
    set_tests_properties(QualificationRunnerPython
        PROPERTIES LABELS "perf;tools" TIMEOUT 120)
    add_test(
        NAME GlDebugLogCheckerPython
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_check_gl_debug_log.py")
    set_tests_properties(GlDebugLogCheckerPython
        PROPERTIES LABELS "perf;tools" TIMEOUT 60)
    add_test(
        NAME ProcessReceiptSummaryPython
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/perf/test_summarize_process_receipts.py")
    set_tests_properties(ProcessReceiptSummaryPython
        PROPERTIES LABELS "perf;tools" TIMEOUT 60)
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
        NAME RepeatQualificationContract
        COMMAND "${Python3_EXECUTABLE}"
                -I -u "${CMAKE_CURRENT_SOURCE_DIR}/qualify/test_repeat_qualification.py" -v)
    set_tests_properties(RepeatQualificationContract
        PROPERTIES LABELS "tools;persistence" TIMEOUT 300
                   ENVIRONMENT "LUMINUMBRA_RQ_CONTRACT_DIAGNOSTIC_DIR=${CMAKE_BINARY_DIR}/Testing/Temporary/rq-contract-failures")
    add_test(
        NAME AcceptanceMatrixContract
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                AcceptanceMatrixTests ProductionAcceptanceMatrixTests
                AcceptanceMatrixPublishabilityTests PublicTreeScanTests)
    set_tests_properties(AcceptanceMatrixContract
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixPublicTreeContract1
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                AcceptanceMatrixPublicTreeTests.test_partition_0)
    add_test(
        NAME AcceptanceMatrixPublicTreeContract2
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                AcceptanceMatrixPublicTreeTests.test_partition_1)
    add_test(
        NAME AcceptanceMatrixPublicTreeContract3
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                AcceptanceMatrixPublicTreeTests.test_partition_2)
    add_test(
        NAME AcceptanceMatrixPublicTreeContract4
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                AcceptanceMatrixPublicTreeTests.test_partition_3)
    set_tests_properties(
        AcceptanceMatrixPublicTreeContract1 AcceptanceMatrixPublicTreeContract2
        AcceptanceMatrixPublicTreeContract3 AcceptanceMatrixPublicTreeContract4
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixTree
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/check_acceptance_matrix.py"
                --root "${CMAKE_SOURCE_DIR}"
                --check-doc "${CMAKE_SOURCE_DIR}/docs/acceptance-matrix.md")
    set_tests_properties(AcceptanceMatrixTree
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
        NAME SavedWorldRefusalFixtureContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/persistence/test_refusal_fixtures.py")
    set_tests_properties(SavedWorldRefusalFixtureContract
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
        NAME AmbientReceiptComparatorContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_compare_ambient_receipts.py")
    set_tests_properties(AmbientReceiptComparatorContract
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AsanShardContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_asan_shards.py")
    set_tests_properties(AsanShardContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    # Disjoint shard-contract partitions retain the complete suite and deadline.
    add_test(
        NAME CTestShardContractPart1
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 1)
    set_tests_properties(CTestShardContractPart1
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME CTestShardContractPart2
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 2)
    set_tests_properties(CTestShardContractPart2
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME CTestShardContractPart3
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 3)
    set_tests_properties(CTestShardContractPart3
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME CTestShardContractPart4
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 4)
    set_tests_properties(CTestShardContractPart4
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    # Disjoint policy partitions preserve every unittest case and the 30s deadline.
    # Direct Python invocation and unittest discovery still execute the full suite.
    # Partition 0 reserves both slots for its bounded route-grid overlap.
    add_test(
        NAME WorkflowPolicyContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 0 --case-workers 2)
    set_tests_properties(WorkflowPolicyContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30 PROCESSORS 2)
    add_test(
        NAME WorkflowPolicyContractPart1
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 1)
    set_tests_properties(WorkflowPolicyContractPart1
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart2
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 2)
    set_tests_properties(WorkflowPolicyContractPart2
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart3
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 3)
    set_tests_properties(WorkflowPolicyContractPart3
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart4
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 4)
    set_tests_properties(WorkflowPolicyContractPart4
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart5
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 5)
    set_tests_properties(WorkflowPolicyContractPart5
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart6
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 6)
    set_tests_properties(WorkflowPolicyContractPart6
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart7
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 7)
    set_tests_properties(WorkflowPolicyContractPart7
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME SoftwareGlEvidenceContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/static_preview/test_run_software_gl.py")
    set_tests_properties(SoftwareGlEvidenceContract
        PROPERTIES LABELS "tools;rendering" TIMEOUT 30)
    add_test(
        NAME QualificationCaptureContract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_qualification_capture.py")
    set_tests_properties(QualificationCaptureContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME ReleaseVerifyScriptContract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/release/test_verify_release.py")
    set_tests_properties(ReleaseVerifyScriptContract
        PROPERTIES LABELS "release;tools" TIMEOUT 60)
    add_test(
        NAME ReleaseGatesScriptContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/release/test_validate_release_gates.py"
                RecordTests ApiTests StatusTests GitCommandTests WiringTests)
    set_tests_properties(ReleaseGatesScriptContract
        PROPERTIES LABELS "release;tools" TIMEOUT 60)
    add_test(
        NAME ReleaseGatesGitContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/release/test_validate_release_gates.py" GitTests)
    set_tests_properties(ReleaseGatesGitContract
        PROPERTIES LABELS "release;tools" TIMEOUT 60)
    add_test(
        NAME ReleaseGatesCliContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/release/test_validate_release_gates.py" CliTests)
    set_tests_properties(ReleaseGatesCliContract
        PROPERTIES LABELS "release;tools" TIMEOUT 60)
    add_test(
        NAME ReleaseGatesProtectionContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/release/test_validate_release_gates.py" ProtectionTests)
    set_tests_properties(ReleaseGatesProtectionContract
        PROPERTIES LABELS "release;tools" TIMEOUT 60)
    add_test(
        NAME SlowestTestsScriptContract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_slowest_tests.py")
    set_tests_properties(SlowestTestsScriptContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    include("${CMAKE_CURRENT_SOURCE_DIR}/ci/local_check_contracts.cmake")
    add_test(
        NAME LocalCheckRegistrationContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_check_contracts.py")
    set_tests_properties(LocalCheckRegistrationContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
endif()
