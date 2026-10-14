# Called post-build with quoted DLL lists; an empty target list is valid when
# imported libraries do not expose their runtime location to CMake.
set(_runtime_dlls ${NETWORK_TARGET_DLLS} ${NETWORK_EXTRA_DLLS})
list(REMOVE_DUPLICATES _runtime_dlls)
# Script mode has no configured compiler. Prefer the build's binary inspector
# for both backends on every supported CMake version.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
if(NETWORK_DUMPBIN)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${NETWORK_DUMPBIN}")
elseif(NETWORK_OBJDUMP)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${NETWORK_OBJDUMP}")
endif()
set(_search_dirs ${NETWORK_SEARCH_DIRS})
foreach(_dll IN LISTS _runtime_dlls)
    get_filename_component(_bin "${_dll}" DIRECTORY)
    list(APPEND _search_dirs "${_bin}")
endforeach()
list(REMOVE_DUPLICATES _search_dirs)
set(_system_regexes
    ".*[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
    ".*[/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\].*"
    ".*[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm][/\\\\].*")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${NETWORK_EXECUTABLE}"
    LIBRARIES ${_runtime_dlls}
    DIRECTORIES ${_search_dirs}
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES "^api-ms-.*" "^ext-ms-.*"
    POST_EXCLUDE_REGEXES ${_system_regexes})
if(_unresolved)
    list(SORT _unresolved)
    list(JOIN _unresolved "\n  " _unresolved_imports)
    message(FATAL_ERROR "Unresolved non-system networking runtime imports for ${NETWORK_EXECUTABLE}:\n  ${_unresolved_imports}\nSearched directories: ${_search_dirs}")
endif()
list(APPEND _runtime_dlls ${_resolved})
list(REMOVE_DUPLICATES _runtime_dlls)
# Apply the same system-directory exclusions to supplemental target/explicit DLLs.
foreach(_regex IN LISTS _system_regexes)
    list(FILTER _runtime_dlls EXCLUDE REGEX "${_regex}")
endforeach()
if(_runtime_dlls)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_runtime_dlls} "${NETWORK_RUNTIME_DIR}"
        RESULT_VARIABLE _copy_result)
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "Failed to copy networking runtime DLLs next to ${NETWORK_EXECUTABLE}")
    endif()
endif()
