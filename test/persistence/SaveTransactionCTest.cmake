# GoogleTest discovery strips DISABLED_ from the CTest name and marks the test
# disabled. It still appears as unevaluated in JUnit unless excluded by label.
# The child exists only on Windows and is exercised by its parent process test.
list(FIND frontier_gates_test_TESTS "SaveTransaction.WindowsInterruptionChild" _lum_txn_child_index)
if(NOT _lum_txn_child_index EQUAL -1)
    set_tests_properties(SaveTransaction.WindowsInterruptionChild PROPERTIES
        LABELS "manual;helper")
endif()
unset(_lum_txn_child_index)
