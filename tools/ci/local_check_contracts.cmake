# Shared by the engine and the compiler-free hosted CTest project. Keep a
# single registration so either CI route exercises the shipping deadlines.
set(_local_check_source_dir "${CMAKE_CURRENT_LIST_DIR}")
add_test(
    NAME ChangedTranslationUnitsContract
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_local_check_source_dir}/test_changed_tus.py")
add_test(
    NAME OfflineDependenciesContract
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_local_check_source_dir}/test_offline_deps.py")
set_tests_properties(ChangedTranslationUnitsContract OfflineDependenciesContract
    PROPERTIES LABELS "ci;tools" TIMEOUT 30)
