# tools/CMakeLists.txt

add_executable(asset_processor
    asset_processor.cpp
    morph_asset_processor.cpp
    "${CMAKE_SOURCE_DIR}/src/luminumbra_common/animation/MorphMesh.cpp"
    "${CMAKE_SOURCE_DIR}/src/luminumbra_common/core/FilesystemPath.cpp"
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
        NAME ContainmentLinkClosureContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_containment_link_closure.py")
    set_tests_properties(ContainmentLinkClosureContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME FirstPartyExceptionsContract
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_first_party_exceptions.py")
    set_tests_properties(FirstPartyExceptionsContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
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
                --contract-partition 0)
    set_tests_properties(AcceptanceMatrixContract
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract2
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 1)
    set_tests_properties(AcceptanceMatrixContract2
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract3
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 2)
    set_tests_properties(AcceptanceMatrixContract3
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract4
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 3)
    set_tests_properties(AcceptanceMatrixContract4
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract5
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 4)
    set_tests_properties(AcceptanceMatrixContract5
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract6
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 5)
    set_tests_properties(AcceptanceMatrixContract6
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract7
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 6)
    set_tests_properties(AcceptanceMatrixContract7
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract8
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 7)
    set_tests_properties(AcceptanceMatrixContract8
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract9
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 8)
    set_tests_properties(AcceptanceMatrixContract9
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract10
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 9)
    set_tests_properties(AcceptanceMatrixContract10
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract11
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 10)
    set_tests_properties(AcceptanceMatrixContract11
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract12
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 11)
    set_tests_properties(AcceptanceMatrixContract12
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract13
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 12)
    set_tests_properties(AcceptanceMatrixContract13
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract14
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 13)
    set_tests_properties(AcceptanceMatrixContract14
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract15
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 14)
    set_tests_properties(AcceptanceMatrixContract15
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract16
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 15)
    set_tests_properties(AcceptanceMatrixContract16
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract17
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 16)
    set_tests_properties(AcceptanceMatrixContract17
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract18
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 17)
    set_tests_properties(AcceptanceMatrixContract18
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract19
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 18)
    set_tests_properties(AcceptanceMatrixContract19
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract20
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 19)
    set_tests_properties(AcceptanceMatrixContract20
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract21
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 20)
    set_tests_properties(AcceptanceMatrixContract21
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract22
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 21)
    set_tests_properties(AcceptanceMatrixContract22
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract23
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 22)
    set_tests_properties(AcceptanceMatrixContract23
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract24
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 23)
    set_tests_properties(AcceptanceMatrixContract24
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract25
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 24)
    set_tests_properties(AcceptanceMatrixContract25
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract26
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 25)
    set_tests_properties(AcceptanceMatrixContract26
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract27
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 26)
    set_tests_properties(AcceptanceMatrixContract27
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract28
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 27)
    set_tests_properties(AcceptanceMatrixContract28
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract29
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 28)
    set_tests_properties(AcceptanceMatrixContract29
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract30
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 29)
    set_tests_properties(AcceptanceMatrixContract30
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract31
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 30)
    set_tests_properties(AcceptanceMatrixContract31
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract32
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 31)
    set_tests_properties(AcceptanceMatrixContract32
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract33
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 32)
    set_tests_properties(AcceptanceMatrixContract33
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract34
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 33)
    set_tests_properties(AcceptanceMatrixContract34
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract35
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 34)
    set_tests_properties(AcceptanceMatrixContract35
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract36
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 35)
    set_tests_properties(AcceptanceMatrixContract36
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract37
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 36)
    set_tests_properties(AcceptanceMatrixContract37
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract38
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 37)
    set_tests_properties(AcceptanceMatrixContract38
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract39
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 38)
    set_tests_properties(AcceptanceMatrixContract39
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract40
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 39)
    set_tests_properties(AcceptanceMatrixContract40
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract41
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 40)
    set_tests_properties(AcceptanceMatrixContract41
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract42
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 41)
    set_tests_properties(AcceptanceMatrixContract42
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract43
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 42)
    set_tests_properties(AcceptanceMatrixContract43
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract44
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 43)
    set_tests_properties(AcceptanceMatrixContract44
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract45
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 44)
    set_tests_properties(AcceptanceMatrixContract45
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract46
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 45)
    set_tests_properties(AcceptanceMatrixContract46
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract47
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 46)
    set_tests_properties(AcceptanceMatrixContract47
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract48
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 47)
    set_tests_properties(AcceptanceMatrixContract48
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract49
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 48)
    set_tests_properties(AcceptanceMatrixContract49
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract50
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 49)
    set_tests_properties(AcceptanceMatrixContract50
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract51
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 50)
    set_tests_properties(AcceptanceMatrixContract51
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract52
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 51)
    set_tests_properties(AcceptanceMatrixContract52
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract53
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 52)
    set_tests_properties(AcceptanceMatrixContract53
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract54
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 53)
    set_tests_properties(AcceptanceMatrixContract54
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract55
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 54)
    set_tests_properties(AcceptanceMatrixContract55
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract56
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 55)
    set_tests_properties(AcceptanceMatrixContract56
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract57
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 56)
    set_tests_properties(AcceptanceMatrixContract57
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract58
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 57)
    set_tests_properties(AcceptanceMatrixContract58
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract59
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 58)
    set_tests_properties(AcceptanceMatrixContract59
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract60
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 59)
    set_tests_properties(AcceptanceMatrixContract60
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract61
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 60)
    set_tests_properties(AcceptanceMatrixContract61
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract62
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 61)
    set_tests_properties(AcceptanceMatrixContract62
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract63
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 62)
    set_tests_properties(AcceptanceMatrixContract63
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract64
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 63)
    set_tests_properties(AcceptanceMatrixContract64
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract65
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 64)
    set_tests_properties(AcceptanceMatrixContract65
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract66
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 65)
    set_tests_properties(AcceptanceMatrixContract66
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract67
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 66)
    set_tests_properties(AcceptanceMatrixContract67
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract68
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 67)
    set_tests_properties(AcceptanceMatrixContract68
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract69
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 68)
    set_tests_properties(AcceptanceMatrixContract69
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract70
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 69)
    set_tests_properties(AcceptanceMatrixContract70
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract71
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 70)
    set_tests_properties(AcceptanceMatrixContract71
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract72
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 71)
    set_tests_properties(AcceptanceMatrixContract72
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract73
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 72)
    set_tests_properties(AcceptanceMatrixContract73
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract74
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 73)
    set_tests_properties(AcceptanceMatrixContract74
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract75
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 74)
    set_tests_properties(AcceptanceMatrixContract75
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract76
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 75)
    set_tests_properties(AcceptanceMatrixContract76
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract77
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 76)
    set_tests_properties(AcceptanceMatrixContract77
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract78
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 77)
    set_tests_properties(AcceptanceMatrixContract78
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract79
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 78)
    set_tests_properties(AcceptanceMatrixContract79
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract80
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 79)
    set_tests_properties(AcceptanceMatrixContract80
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract81
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 80)
    set_tests_properties(AcceptanceMatrixContract81
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract82
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 81)
    set_tests_properties(AcceptanceMatrixContract82
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract83
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 82)
    set_tests_properties(AcceptanceMatrixContract83
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract84
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 83)
    set_tests_properties(AcceptanceMatrixContract84
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract85
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 84)
    set_tests_properties(AcceptanceMatrixContract85
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract86
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 85)
    set_tests_properties(AcceptanceMatrixContract86
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract87
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 86)
    set_tests_properties(AcceptanceMatrixContract87
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract88
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 87)
    set_tests_properties(AcceptanceMatrixContract88
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract89
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 88)
    set_tests_properties(AcceptanceMatrixContract89
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract90
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 89)
    set_tests_properties(AcceptanceMatrixContract90
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract91
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 90)
    set_tests_properties(AcceptanceMatrixContract91
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract92
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 91)
    set_tests_properties(AcceptanceMatrixContract92
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract93
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 92)
    set_tests_properties(AcceptanceMatrixContract93
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract94
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 93)
    set_tests_properties(AcceptanceMatrixContract94
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract95
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 94)
    set_tests_properties(AcceptanceMatrixContract95
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract96
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 95)
    set_tests_properties(AcceptanceMatrixContract96
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract97
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 96)
    set_tests_properties(AcceptanceMatrixContract97
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract98
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 97)
    set_tests_properties(AcceptanceMatrixContract98
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract99
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 98)
    set_tests_properties(AcceptanceMatrixContract99
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract100
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 99)
    set_tests_properties(AcceptanceMatrixContract100
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract101
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 100)
    set_tests_properties(AcceptanceMatrixContract101
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract102
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 101)
    set_tests_properties(AcceptanceMatrixContract102
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract103
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 102)
    set_tests_properties(AcceptanceMatrixContract103
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract104
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 103)
    set_tests_properties(AcceptanceMatrixContract104
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract105
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 104)
    set_tests_properties(AcceptanceMatrixContract105
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract106
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 105)
    set_tests_properties(AcceptanceMatrixContract106
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract107
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 106)
    set_tests_properties(AcceptanceMatrixContract107
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract108
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 107)
    set_tests_properties(AcceptanceMatrixContract108
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract109
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 108)
    set_tests_properties(AcceptanceMatrixContract109
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract110
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 109)
    set_tests_properties(AcceptanceMatrixContract110
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract111
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 110)
    set_tests_properties(AcceptanceMatrixContract111
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract112
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 111)
    set_tests_properties(AcceptanceMatrixContract112
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract113
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 112)
    set_tests_properties(AcceptanceMatrixContract113
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract114
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 113)
    set_tests_properties(AcceptanceMatrixContract114
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract115
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 114)
    set_tests_properties(AcceptanceMatrixContract115
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract116
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 115)
    set_tests_properties(AcceptanceMatrixContract116
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract117
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 116)
    set_tests_properties(AcceptanceMatrixContract117
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract118
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 117)
    set_tests_properties(AcceptanceMatrixContract118
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract119
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 118)
    set_tests_properties(AcceptanceMatrixContract119
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract120
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 119)
    set_tests_properties(AcceptanceMatrixContract120
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract121
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 120)
    set_tests_properties(AcceptanceMatrixContract121
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract122
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 121)
    set_tests_properties(AcceptanceMatrixContract122
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract123
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 122)
    set_tests_properties(AcceptanceMatrixContract123
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract124
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 123)
    set_tests_properties(AcceptanceMatrixContract124
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract125
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 124)
    set_tests_properties(AcceptanceMatrixContract125
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract126
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 125)
    set_tests_properties(AcceptanceMatrixContract126
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract127
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 126)
    set_tests_properties(AcceptanceMatrixContract127
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract128
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 127)
    set_tests_properties(AcceptanceMatrixContract128
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract129
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 128)
    set_tests_properties(AcceptanceMatrixContract129
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract130
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 129)
    set_tests_properties(AcceptanceMatrixContract130
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract131
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 130)
    set_tests_properties(AcceptanceMatrixContract131
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract132
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 131)
    set_tests_properties(AcceptanceMatrixContract132
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract133
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 132)
    set_tests_properties(AcceptanceMatrixContract133
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract134
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 133)
    set_tests_properties(AcceptanceMatrixContract134
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract135
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 134)
    set_tests_properties(AcceptanceMatrixContract135
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract136
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 135)
    set_tests_properties(AcceptanceMatrixContract136
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract137
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 136)
    set_tests_properties(AcceptanceMatrixContract137
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract138
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 137)
    set_tests_properties(AcceptanceMatrixContract138
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract139
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 138)
    set_tests_properties(AcceptanceMatrixContract139
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract140
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 139)
    set_tests_properties(AcceptanceMatrixContract140
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract141
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 140)
    set_tests_properties(AcceptanceMatrixContract141
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract142
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 141)
    set_tests_properties(AcceptanceMatrixContract142
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract143
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 142)
    set_tests_properties(AcceptanceMatrixContract143
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract144
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 143)
    set_tests_properties(AcceptanceMatrixContract144
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract145
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 144)
    set_tests_properties(AcceptanceMatrixContract145
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract146
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 145)
    set_tests_properties(AcceptanceMatrixContract146
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract147
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 146)
    set_tests_properties(AcceptanceMatrixContract147
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract148
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 147)
    set_tests_properties(AcceptanceMatrixContract148
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract149
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 148)
    set_tests_properties(AcceptanceMatrixContract149
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract150
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 149)
    set_tests_properties(AcceptanceMatrixContract150
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract151
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 150)
    set_tests_properties(AcceptanceMatrixContract151
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract152
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 151)
    set_tests_properties(AcceptanceMatrixContract152
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract153
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 152)
    set_tests_properties(AcceptanceMatrixContract153
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract154
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 153)
    set_tests_properties(AcceptanceMatrixContract154
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract155
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 154)
    set_tests_properties(AcceptanceMatrixContract155
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract156
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 155)
    set_tests_properties(AcceptanceMatrixContract156
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract157
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 156)
    set_tests_properties(AcceptanceMatrixContract157
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract158
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 157)
    set_tests_properties(AcceptanceMatrixContract158
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract159
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 158)
    set_tests_properties(AcceptanceMatrixContract159
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract160
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 159)
    set_tests_properties(AcceptanceMatrixContract160
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract161
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 160)
    set_tests_properties(AcceptanceMatrixContract161
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract162
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 161)
    set_tests_properties(AcceptanceMatrixContract162
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract163
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 162)
    set_tests_properties(AcceptanceMatrixContract163
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract164
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 163)
    set_tests_properties(AcceptanceMatrixContract164
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract165
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 164)
    set_tests_properties(AcceptanceMatrixContract165
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract166
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 165)
    set_tests_properties(AcceptanceMatrixContract166
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract167
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 166)
    set_tests_properties(AcceptanceMatrixContract167
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract168
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 167)
    set_tests_properties(AcceptanceMatrixContract168
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract169
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 168)
    set_tests_properties(AcceptanceMatrixContract169
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract170
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 169)
    set_tests_properties(AcceptanceMatrixContract170
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract171
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 170)
    set_tests_properties(AcceptanceMatrixContract171
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract172
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 171)
    set_tests_properties(AcceptanceMatrixContract172
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract173
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 172)
    set_tests_properties(AcceptanceMatrixContract173
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract174
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 173)
    set_tests_properties(AcceptanceMatrixContract174
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract175
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 174)
    set_tests_properties(AcceptanceMatrixContract175
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract176
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 175)
    set_tests_properties(AcceptanceMatrixContract176
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract177
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 176)
    set_tests_properties(AcceptanceMatrixContract177
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract178
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 177)
    set_tests_properties(AcceptanceMatrixContract178
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract179
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 178)
    set_tests_properties(AcceptanceMatrixContract179
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract180
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 179)
    set_tests_properties(AcceptanceMatrixContract180
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract181
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 180)
    set_tests_properties(AcceptanceMatrixContract181
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract182
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 181)
    set_tests_properties(AcceptanceMatrixContract182
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract183
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 182)
    set_tests_properties(AcceptanceMatrixContract183
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract184
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 183)
    set_tests_properties(AcceptanceMatrixContract184
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract185
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 184)
    set_tests_properties(AcceptanceMatrixContract185
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract186
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 185)
    set_tests_properties(AcceptanceMatrixContract186
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract187
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 186)
    set_tests_properties(AcceptanceMatrixContract187
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract188
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 187)
    set_tests_properties(AcceptanceMatrixContract188
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract189
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 188)
    set_tests_properties(AcceptanceMatrixContract189
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract190
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 189)
    set_tests_properties(AcceptanceMatrixContract190
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract191
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 190)
    set_tests_properties(AcceptanceMatrixContract191
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract192
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 191)
    set_tests_properties(AcceptanceMatrixContract192
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract193
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 192)
    set_tests_properties(AcceptanceMatrixContract193
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract194
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 193)
    set_tests_properties(AcceptanceMatrixContract194
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract195
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 194)
    set_tests_properties(AcceptanceMatrixContract195
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract196
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 195)
    set_tests_properties(AcceptanceMatrixContract196
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract197
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 196)
    set_tests_properties(AcceptanceMatrixContract197
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract198
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 197)
    set_tests_properties(AcceptanceMatrixContract198
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract199
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 198)
    set_tests_properties(AcceptanceMatrixContract199
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract200
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 199)
    set_tests_properties(AcceptanceMatrixContract200
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract201
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 200)
    set_tests_properties(AcceptanceMatrixContract201
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract202
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 201)
    set_tests_properties(AcceptanceMatrixContract202
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract203
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 202)
    set_tests_properties(AcceptanceMatrixContract203
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract204
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 203)
    set_tests_properties(AcceptanceMatrixContract204
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract205
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 204)
    set_tests_properties(AcceptanceMatrixContract205
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract206
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 205)
    set_tests_properties(AcceptanceMatrixContract206
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract207
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 206)
    set_tests_properties(AcceptanceMatrixContract207
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract208
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 207)
    set_tests_properties(AcceptanceMatrixContract208
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract209
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 208)
    set_tests_properties(AcceptanceMatrixContract209
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract210
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 209)
    set_tests_properties(AcceptanceMatrixContract210
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract211
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 210)
    set_tests_properties(AcceptanceMatrixContract211
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract212
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 211)
    set_tests_properties(AcceptanceMatrixContract212
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract213
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 212)
    set_tests_properties(AcceptanceMatrixContract213
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract214
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 213)
    set_tests_properties(AcceptanceMatrixContract214
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract215
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 214)
    set_tests_properties(AcceptanceMatrixContract215
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract216
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 215)
    set_tests_properties(AcceptanceMatrixContract216
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract217
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 216)
    set_tests_properties(AcceptanceMatrixContract217
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract218
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 217)
    set_tests_properties(AcceptanceMatrixContract218
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract219
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 218)
    set_tests_properties(AcceptanceMatrixContract219
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract220
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 219)
    set_tests_properties(AcceptanceMatrixContract220
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract221
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 220)
    set_tests_properties(AcceptanceMatrixContract221
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract222
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 221)
    set_tests_properties(AcceptanceMatrixContract222
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract223
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 222)
    set_tests_properties(AcceptanceMatrixContract223
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract224
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 223)
    set_tests_properties(AcceptanceMatrixContract224
        PROPERTIES LABELS "hygiene;tools" TIMEOUT 30)
    add_test(
        NAME AcceptanceMatrixContract225
        COMMAND "${Python3_EXECUTABLE}"
                -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_check_acceptance_matrix.py"
                --contract-partition 224)
    set_tests_properties(AcceptanceMatrixContract225
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
    add_test(
        NAME CTestShardContractPart5
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 5)
    set_tests_properties(CTestShardContractPart5
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME CTestShardContractPart6
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 6)
    set_tests_properties(CTestShardContractPart6
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME CTestShardContractPart7
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 7)
    set_tests_properties(CTestShardContractPart7
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME CTestShardContractPart8
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition 8)
    set_tests_properties(CTestShardContractPart8
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    # Disjoint policy partitions preserve every unittest case and the 30s deadline.
    # Direct Python invocation and unittest discovery still execute the full suite.
    # Partition 9 reserves two slots for its bounded route-grid overlap.
    # Partitions 10 and 11 isolate required aggregates and UCRT64 classifiers.
    add_test(
        NAME WorkflowPolicyContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 0)
    set_tests_properties(WorkflowPolicyContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
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
        NAME WorkflowPolicyContractPart8
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 8)
    set_tests_properties(WorkflowPolicyContractPart8
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart9
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 9 --case-workers 2)
    set_tests_properties(WorkflowPolicyContractPart9
        PROPERTIES LABELS "ci;tools" TIMEOUT 30 PROCESSORS 2)
    add_test(
        NAME WorkflowPolicyContractPart10
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 10)
    set_tests_properties(WorkflowPolicyContractPart10
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart11
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 11)
    set_tests_properties(WorkflowPolicyContractPart11
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart12
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 12)
    set_tests_properties(WorkflowPolicyContractPart12
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME WorkflowPolicyContractPart13
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py" --partition 13)
    set_tests_properties(WorkflowPolicyContractPart13
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
    # Every borrowed-input consumer shares custody; recovery cases have separate
    # entries so real Git publication stays inside the existing deadlines.
    set(_rmlui_source_contracts
        RmlUiSourceScriptContract RmlUiSourceCommandContract
        LocalRmlUiRecipeContract LocalRmlUiRecoveryFailureContract)
    add_test(
        NAME RmlUiSourceScriptContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_rmlui_source.py"
                SourceFiles.test_full_pin_publication_and_readonly_repeat)
    set_tests_properties(RmlUiSourceScriptContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
    # Individual file-fixture cases keep real publication inside its deadline.
    foreach(fixture IN ITEMS
            every_changed_file_preserves_operations_defaults_and_enum_declarations
            files_mixed_missing_extra_and_partial_refuse_untouched
            git_pin_index_modes_flags_alternates_and_config_refusals
            mode_and_unreadable_refuse
            nonordinary_files_and_links
            operation_assertions_use_strict_utf8_with_a_cp1252_default
)
        add_test(
            NAME RmlUiSourceFixture_${fixture}
            COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_rmlui_source.py"
                    SourceFiles.test_${fixture})
        set_tests_properties(RmlUiSourceFixture_${fixture}
            PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
        list(APPEND _rmlui_source_contracts RmlUiSourceFixture_${fixture})
    endforeach()
    foreach(boundary IN ITEMS check apply postverify rename receipt admission)
        add_test(
            NAME RmlUiSourceInterruption_${boundary}
            COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_rmlui_source.py"
                    SourceFiles.test_interruption_at_${boundary}_never_marks_incomplete_ready)
        set_tests_properties(RmlUiSourceInterruption_${boundary}
            PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
        list(APPEND _rmlui_source_contracts RmlUiSourceInterruption_${boundary})
    endforeach()
    add_test(
        NAME RmlUiSourceCommandContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_rmlui_source.py"
                DeliveryRefusals PairedWorkflowInitialization HostedConfigurationContracts BootstrapInterpreterBinding
                OwnedCommandLifetime CommandCancellationContract WindowsContainmentRefusal OwnershipFieldDiagnostics RootMismatchObservations
                ConfigureCallerBoundaries Utf8SourceReads FixtureCleanupCustody)
    set_tests_properties(RmlUiSourceCommandContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
    foreach(recovery IN ITEMS
            borrowed_archive_without_git_returns_its_original_metadata
            borrowed_recovery_refuses_unknown_partial_and_changed_metadata
            setup_final_verification_failure_returns_the_complete_input
            teardown_refusal_preserves_ancestor_and_can_retry_both_locations)
        add_test(
            NAME RmlUiSourceRecovery_${recovery}
            COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_rmlui_source.py"
                    BorrowedFixtureRecovery.test_${recovery})
        set_tests_properties(RmlUiSourceRecovery_${recovery}
            PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
        list(APPEND _rmlui_source_contracts RmlUiSourceRecovery_${recovery})
    endforeach()
    add_test(
        NAME LocalRmlUiRecipeContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_rmlui_recipe.py"
                LocalSourceRecipe.test_documented_local_recipe_requires_both_owned_disconnected_sources)
    set_tests_properties(LocalRmlUiRecipeContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
    foreach(recipe IN ITEMS
            worker_configures_both_owned_disconnected_sources
            worker_refuses_invalid_gcc_receipts
            worker_refuses_invalid_tidy_receipts)
        add_test(
            NAME LocalRmlUiRecipe_${recipe}
            COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_rmlui_recipe.py"
                    LocalSourceRecipe.test_${recipe})
        set_tests_properties(LocalRmlUiRecipe_${recipe}
            PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
        list(APPEND _rmlui_source_contracts LocalRmlUiRecipe_${recipe})
    endforeach()
    foreach(boundary IN ITEMS beforepatch afterpatch beforerename afterrename receipt configure)
        add_test(
            NAME LocalRmlUiRecovery_${boundary}
            COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_rmlui_recipe.py"
                    LocalRecipeRecovery.test_publication_${boundary}_returns_borrowed_input)
        set_tests_properties(LocalRmlUiRecovery_${boundary}
            PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
        list(APPEND _rmlui_source_contracts LocalRmlUiRecovery_${boundary})
    endforeach()
    add_test(
        NAME LocalRmlUiRecoveryFailureContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_rmlui_recipe.py"
                LocalRecipeRecovery.test_failed_restoration_preserves_recoverable_input_and_both_errors)
    set_tests_properties(LocalRmlUiRecoveryFailureContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 60 RESOURCE_LOCK rmlui_offline_input)
    add_test(
        NAME SlowestTestsScriptContract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_slowest_tests.py")
    set_tests_properties(SlowestTestsScriptContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    add_test(
        NAME UiLayoutSourceContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ui_layout_contract.py")
    set_tests_properties(UiLayoutSourceContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    include("${CMAKE_CURRENT_SOURCE_DIR}/ci/local_check_contracts.cmake")
    add_test(
        NAME LocalCheckRegistrationContract
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_check_contracts.py")
    set_tests_properties(LocalCheckRegistrationContract
        PROPERTIES LABELS "ci;tools" TIMEOUT 30)
    if(WIN32)
        # Both Windows shard profiles use four CTest slots. Reserve the pool
        # for each Python ci/tools contract, including its child processes.
        # PROCESSORS adds no dependency/lock edges or cross-shard grouping;
        # CTest can always run a reservation alone, even with a smaller -j.
        # Linux retains its existing parallelism. Local-check registrations
        # share this policy through local_check_contracts.cmake.
        set_tests_properties(
            WorldgenProbeSmokeValidatorContract AsanShardContract
            UiLayoutSourceContract
            ContainmentLinkClosureContract FirstPartyExceptionsContract
            CTestShardContractPart1 CTestShardContractPart2
            CTestShardContractPart3 CTestShardContractPart4
            CTestShardContractPart5 CTestShardContractPart6
            CTestShardContractPart7 CTestShardContractPart8
            WorkflowPolicyContract WorkflowPolicyContractPart1
            WorkflowPolicyContractPart2 WorkflowPolicyContractPart3
            WorkflowPolicyContractPart4 WorkflowPolicyContractPart5
            WorkflowPolicyContractPart6 WorkflowPolicyContractPart7
            WorkflowPolicyContractPart8 WorkflowPolicyContractPart9
            WorkflowPolicyContractPart10 WorkflowPolicyContractPart11
            WorkflowPolicyContractPart12 WorkflowPolicyContractPart13
            QualificationCaptureContract SlowestTestsScriptContract
            LocalCheckRegistrationContract
            ${_rmlui_source_contracts}
            PROPERTIES PROCESSORS 4)
        # Explicit costs seed longest-first scheduling on fresh runners.
        set_property(TEST WorkflowPolicyContract PROPERTY COST 30)
        set_property(TEST CTestShardContractPart1 CTestShardContractPart2
            CTestShardContractPart3 CTestShardContractPart4
            CTestShardContractPart5 CTestShardContractPart6
            CTestShardContractPart7 CTestShardContractPart8 PROPERTY COST 25)
        set_property(TEST WorkflowPolicyContractPart1 PROPERTY COST 21)
        set_property(TEST WorkflowPolicyContractPart8 PROPERTY COST 20)
        set_property(TEST WorkflowPolicyContractPart9 PROPERTY COST 19)
        set_property(TEST WorkflowPolicyContractPart10 PROPERTY COST 17)
        set_property(TEST WorkflowPolicyContractPart11 PROPERTY COST 16)
        set_property(TEST WorkflowPolicyContractPart12 PROPERTY COST 15)
        set_property(TEST WorkflowPolicyContractPart13 PROPERTY COST 14)
    endif()
endif()
