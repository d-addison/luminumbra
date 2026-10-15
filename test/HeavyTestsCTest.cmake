# Label only the reviewed long cases; quick world-opening refusals stay light.
# The TEST_LIST is populated by the preceding GoogleTest discovery includes.
foreach(_lum_heavy_variant IN ITEMS Disabled Enabled)
    set(_lum_heavy_name
        "ActiveRegions/SimBudgetWorldParity.Populated3600TicksPreserveWorldHashAndReplayWork/${_lum_heavy_variant}")
    list(FIND LUMINUMBRA_WORLD_OPEN_TESTS "${_lum_heavy_name}" _lum_heavy_index)
    if(_lum_heavy_index EQUAL -1)
        message(FATAL_ERROR "Reviewed heavy correctness case missing: ${_lum_heavy_name}")
    endif()
    # CTest's handler appends LABELS; CMake's TEST object API is unavailable here.
    set_tests_properties("${_lum_heavy_name}" PROPERTIES LABELS heavy)
endforeach()
unset(_lum_heavy_index)
unset(_lum_heavy_name)
unset(_lum_heavy_variant)
