# Shared by the engine and the compiler-free hosted CTest project. Keep a
# single registration so either CI route exercises the shipping deadlines.
set(_local_check_source_dir "${CMAKE_CURRENT_LIST_DIR}")
get_property(_local_check_existing_tests DIRECTORY PROPERTY TESTS)
add_test(
    NAME ChangedTranslationUnitsContract
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_local_check_source_dir}/test_changed_tus.py")
set_tests_properties(ChangedTranslationUnitsContract
    PROPERTIES LABELS "ci;tools" TIMEOUT 30)
# Disjoint partitions preserve every unittest case and the 30s deadline.
add_test(
    NAME OfflineDependenciesContract
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_local_check_source_dir}/test_offline_deps.py" --partition 0)
set_tests_properties(OfflineDependenciesContract
    PROPERTIES LABELS "ci;tools" TIMEOUT 30)
add_test(
    NAME OfflineDependenciesContractPart1
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_local_check_source_dir}/test_offline_deps.py" --partition 1)
set_tests_properties(OfflineDependenciesContractPart1
    PROPERTIES LABELS "ci;tools" TIMEOUT 30)
add_test(
    NAME OfflineDependenciesContractPart2
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_local_check_source_dir}/test_offline_deps.py" --partition 2)
set_tests_properties(OfflineDependenciesContractPart2
    PROPERTIES LABELS "ci;tools" TIMEOUT 30)
if(WIN32)
    # Match the engine's four-slot Windows pool reservation in both projects.
    get_property(_local_contracts DIRECTORY PROPERTY TESTS)
    foreach(_contract IN LISTS _local_contracts)
        get_test_property(${_contract} LABELS _contract_labels)
        if(NOT _contract IN_LIST _local_check_existing_tests
                AND "ci" IN_LIST _contract_labels AND "tools" IN_LIST _contract_labels)
            set_tests_properties(${_contract} PROPERTIES PROCESSORS 4)
        endif()
    endforeach()
    set_property(TEST ChangedTranslationUnitsContract PROPERTY COST 29)
    set_property(TEST OfflineDependenciesContractPart2 PROPERTY COST 18)
    set_property(TEST OfflineDependenciesContract OfflineDependenciesContractPart1 PROPERTY COST 10)
endif()
