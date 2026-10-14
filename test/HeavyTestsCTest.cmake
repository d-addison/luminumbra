# Discovery decorates parameterized names with GetParam() comments. Apply COST
# after discovery, using its actual names. These weights only change scheduling;
# they do not change labels, inclusion, serial execution or timeouts.
set(_lum_discovered_tests ${world_open_refusal_test_TESTS} ${far_lod_worker_test_TESTS})
foreach(_lum_test IN LISTS _lum_discovered_tests)
    if(_lum_test MATCHES "^ActiveRegions/SimBudgetWorldParity\\.Populated3600Ticks(PreserveWorldHashAndSaveToggle|RepeatTracedHashAndWork)/Enabled")
        set_tests_properties("${_lum_test}" PROPERTIES COST 2000)
    elseif(_lum_test MATCHES "^ActiveRegions/SimBudgetWorldParity\\.Populated3600Ticks(PreserveWorldHashAndSaveToggle|RepeatTracedHashAndWork)/Disabled")
        set_tests_properties("${_lum_test}" PROPERTIES COST 697)
    elseif(_lum_test STREQUAL "FarLodWorker.CameraRegionCoveragePreservesLiveTerrainAndWaterOwnership")
        set_tests_properties("${_lum_test}" PROPERTIES COST 229)
    endif()
endforeach()
unset(_lum_test)
unset(_lum_discovered_tests)
