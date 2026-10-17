# Label only the reviewed heavy cases; quick world-opening refusals stay light.
# The TEST_LIST variables are populated by the preceding GoogleTest discovery includes.
# COST hints use measured ASan seconds, rounded to the upper observed duration.
# They prioritize long cases in fresh build trees without changing selection.
foreach(_lum_heavy_variant IN ITEMS Disabled Enabled)
    set(_lum_heavy_name
        "ActiveRegions/SimBudgetWorldParity.Populated3600TicksPreserveWorldHashAndReplayWork/${_lum_heavy_variant}")
    list(FIND LUMINUMBRA_WORLD_OPEN_TESTS "${_lum_heavy_name}" _lum_heavy_index)
    if(_lum_heavy_index EQUAL -1)
        message(FATAL_ERROR "Reviewed heavy correctness case missing: ${_lum_heavy_name}")
    endif()
    # CTest's handler appends LABELS; CMake's TEST object API is unavailable here.
    set_tests_properties("${_lum_heavy_name}" PROPERTIES LABELS heavy COST 830)
endforeach()
unset(_lum_heavy_index)
unset(_lum_heavy_name)
unset(_lum_heavy_variant)

# These long cases remain light. Add only COST, preserving all other properties.
foreach(_lum_cost_case IN ITEMS
    "far_lod_worker_test_TESTS;FarLodWorker.CameraRegionCoveragePreservesLiveTerrainAndWaterOwnership;180"
    "common_tests_TESTS;FarWorldCaveSelection.ProductionFacadeAdjacentTilesShareBordersHalosAndVerticalBrickSamples;125"
    "common_tests_TESTS;WaterDeterminism.WaterResolutionMigrationConvergesAtLoadBothWays;120")
    list(GET _lum_cost_case 0 _lum_cost_test_list)
    list(GET _lum_cost_case 1 _lum_cost_name)
    list(GET _lum_cost_case 2 _lum_cost_seconds)
    list(FIND ${_lum_cost_test_list} "${_lum_cost_name}" _lum_cost_index)
    if(_lum_cost_index EQUAL -1)
        message(FATAL_ERROR "Reviewed long correctness case missing: ${_lum_cost_name}")
    endif()
    set_tests_properties("${_lum_cost_name}" PROPERTIES COST "${_lum_cost_seconds}")
endforeach()
unset(_lum_cost_index)
unset(_lum_cost_seconds)
unset(_lum_cost_name)
unset(_lum_cost_test_list)
unset(_lum_cost_case)
