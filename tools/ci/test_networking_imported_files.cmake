cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/networking_runtime.cmake")

# Script mode cannot create targets. Replace only CMake's target-property reads
# with an in-memory property table; run the production selection function intact.
# Missing properties follow get_target_property's NOTFOUND convention, and SET
# distinguishes an explicitly empty configuration mapping from an unset one.
function(get_target_property OUT_VAR TARGET_NAME PROPERTY)
    _get_property(_set GLOBAL PROPERTY "fixture_${TARGET_NAME}_${PROPERTY}" SET)
    if(_set)
        _get_property(_value GLOBAL PROPERTY "fixture_${TARGET_NAME}_${PROPERTY}")
    else()
        set(_value "${OUT_VAR}-NOTFOUND")
    endif()
    set(${OUT_VAR} "${_value}" PARENT_SCOPE)
endfunction()

function(get_property OUT_VAR)
    if(NOT ARGC EQUAL 6 OR NOT ARGV1 STREQUAL "TARGET"
       OR NOT ARGV3 STREQUAL "PROPERTY" OR NOT ARGV5 STREQUAL "SET")
        message(FATAL_ERROR "Unexpected target-property query: ${ARGV}")
    endif()
    _get_property(_set GLOBAL PROPERTY "fixture_${ARGV2}_${ARGV4}" SET)
    set(${OUT_VAR} "${_set}" PARENT_SCOPE)
endfunction()

function(fixture_properties TARGET_NAME)
    math(EXPR _remainder "${ARGC} % 2")
    if(NOT _remainder EQUAL 1)
        message(FATAL_ERROR "Fixture properties require name/value pairs")
    endif()
    # Read ARGV slots directly to preserve empty and semicolon-list values.
    set(_index 1)
    while(_index LESS ARGC)
        set(_property "${ARGV${_index}}")
        math(EXPR _index "${_index} + 1")
        set_property(GLOBAL PROPERTY "fixture_${TARGET_NAME}_${_property}" "${ARGV${_index}}")
        math(EXPR _index "${_index} + 1")
    endwhile()
endfunction()

function(assert_files TARGET_NAME CONFIG EXPECTED_DLL EXPECTED_IMPLIB)
    # Also catch a function that leaves stale output variables on an empty result.
    set(_dll stale)
    set(_implib stale)
    luminumbra_network_imported_files(_dll _implib "${TARGET_NAME}" "${CONFIG}")
    if(NOT "${_dll}" STREQUAL "${EXPECTED_DLL}"
       OR NOT "${_implib}" STREQUAL "${EXPECTED_IMPLIB}")
        message(FATAL_ERROR
            "${TARGET_NAME} ${CONFIG}: expected DLL '${EXPECTED_DLL}' and import library '${EXPECTED_IMPLIB}', got '${_dll}' and '${_implib}'")
    endif()
endfunction()

# These deliberately nonexistent Windows paths need no SDK, compiler or files.
set(_sdk "C:/Networking SDK with spaces")
if(TEST_UNMATCHED_MAPPING)
    fixture_properties(unmatched TYPE SHARED_LIBRARY
        MAP_IMPORTED_CONFIG_DEBUG Missing
        IMPORTED_LOCATION "${_sdk}/bin/backend.dll"
        IMPORTED_IMPLIB "${_sdk}/lib/backend.lib"
        IMPORTED_CONFIGURATIONS Release
        IMPORTED_LOCATION_RELEASE "${_sdk}/release/bin/backend.dll")
    # An explicit mapping must not silently use generic or unrelated configs.
    luminumbra_network_imported_files(_dll _implib unmatched Debug)
    message(FATAL_ERROR "Unmatched configuration mapping was accepted")
endif()

foreach(_extension IN ITEMS lib dll.a)
    set(_target "backend_${_extension}")
    fixture_properties("${_target}" TYPE SHARED_LIBRARY
        IMPORTED_CONFIGURATIONS "Release;Debug"
        IMPORTED_LOCATION_DEBUG "${_sdk}/debug/bin/backend.dll"
        IMPORTED_IMPLIB_DEBUG "${_sdk}/debug/lib/backend.${_extension}"
        IMPORTED_LOCATION_RELEASE "${_sdk}/release/bin/backend.dll"
        IMPORTED_IMPLIB_RELEASE "${_sdk}/release/lib/backend.${_extension}")
    foreach(_config IN ITEMS Debug Release)
        string(TOLOWER "${_config}" _directory)
        assert_files("${_target}" "${_config}"
            "${_sdk}/${_directory}/bin/backend.dll"
            "${_sdk}/${_directory}/lib/backend.${_extension}")
    endforeach()
    # Package configuration names are case-insensitive; ordered mappings select
    # the first available pair, keeping DLL and import library together.
    fixture_properties("${_target}" MAP_IMPORTED_CONFIG_RELWITHDEBINFO "Missing;ReLeAsE;Debug")
    assert_files("${_target}" RelWithDebInfo
        "${_sdk}/release/bin/backend.dll" "${_sdk}/release/lib/backend.${_extension}")
    assert_files("${_target}" dEbUg
        "${_sdk}/debug/bin/backend.dll" "${_sdk}/debug/lib/backend.${_extension}")
    assert_files("${_target}" MinSizeRel
        "${_sdk}/release/bin/backend.dll" "${_sdk}/release/lib/backend.${_extension}")
    fixture_properties("${_target}" MAP_IMPORTED_CONFIG_RELEASE Debug)
    assert_files("${_target}" Release
        "${_sdk}/debug/bin/backend.dll" "${_sdk}/debug/lib/backend.${_extension}")
endforeach()

# Configuration-independent Windows SDK targets use IMPORTED_IMPLIB for the
# linker and IMPORTED_LOCATION for the runtime DLL in both build configurations.
fixture_properties(sdk TYPE SHARED_LIBRARY
    IMPORTED_LOCATION "${_sdk}/bin/backend.dll"
    IMPORTED_IMPLIB "${_sdk}/lib/backend.lib")
fixture_properties(generic TYPE SHARED_LIBRARY
    IMPORTED_LOCATION "${_sdk}/bin/backend.dll"
    IMPORTED_IMPLIB "${_sdk}/lib/backend.lib"
    IMPORTED_LOCATION_DEBUG "${_sdk}/debug/bin/backend.dll"
    IMPORTED_IMPLIB_DEBUG "${_sdk}/debug/lib/backend.lib"
    MAP_IMPORTED_CONFIG_DEBUG ""
    MAP_IMPORTED_CONFIG_RELEASE "Missing;")
foreach(_config IN ITEMS Debug Release)
    assert_files(sdk "${_config}" "${_sdk}/bin/backend.dll" "${_sdk}/lib/backend.lib")
    assert_files(generic "${_config}" "${_sdk}/bin/backend.dll" "${_sdk}/lib/backend.lib")
endforeach()

# CMake fills a missing half of a selected pair from generic properties only.
fixture_properties(partial TYPE SHARED_LIBRARY
    IMPORTED_LOCATION "${_sdk}/bin/backend.dll"
    IMPORTED_IMPLIB "${_sdk}/lib/backend.lib"
    IMPORTED_LOCATION_DEBUG "${_sdk}/debug/bin/backend.dll"
    IMPORTED_IMPLIB_RELEASE "${_sdk}/release/lib/backend.lib")
assert_files(partial Debug "${_sdk}/debug/bin/backend.dll" "${_sdk}/lib/backend.lib")
assert_files(partial Release "${_sdk}/bin/backend.dll" "${_sdk}/release/lib/backend.lib")
assert_files(partial NOCONFIG "${_sdk}/bin/backend.dll" "${_sdk}/lib/backend.lib")

fixture_properties(interface TYPE INTERFACE_LIBRARY
    IMPORTED_LOCATION "${_sdk}/unused.dll" IMPORTED_IMPLIB "${_sdk}/unused.lib")
fixture_properties(empty TYPE SHARED_LIBRARY)
foreach(_config IN ITEMS Debug Release)
    assert_files(interface "${_config}" "" "")
    assert_files(empty "${_config}" "" "")
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -DTEST_UNMATCHED_MAPPING=ON
    -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
if("${_result}" STREQUAL "0" OR NOT "${_stdout}\n${_stderr}" MATCHES
   "GNS imported target unmatched has no files matching[ \r\n]+MAP_IMPORTED_CONFIG_DEBUG")
    message(FATAL_ERROR "Expected unmatched mapping refusal, got ${_result}:\n${_stdout}\n${_stderr}")
endif()

message(STATUS "Networking imported file mapping contract passed")
