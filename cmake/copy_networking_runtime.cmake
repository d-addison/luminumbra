# The shared target copies DLLs; executable post-build calls only verify them.
# A generated, configuration-specific file preserves paths with spaces and
# avoids adding executable dependencies to their own staging prerequisite.
file(STRINGS "${NETWORK_INPUT_FILE}" _runtime_dlls)
list(FILTER _runtime_dlls EXCLUDE REGEX "^$")
list(REMOVE_DUPLICATES _runtime_dlls)
if(NOT NETWORK_CONFIG)
    set(NETWORK_CONFIG NOCONFIG)
endif()
function(luminumbra_reject_network_basename_conflicts)
    foreach(_dll IN LISTS ARGN)
        get_filename_component(_name "${_dll}" NAME)
        string(TOLOWER "${_name}" _name_key)
        string(SHA256 _key "${_name_key}")
        file(REAL_PATH "${_dll}" _source)
        string(TOLOWER "${_source}" _source_key)
        if(DEFINED _source_key_${_key} AND NOT "${_source_key_${_key}}" STREQUAL "${_source_key}")
            # Verification can resolve both the package source and its staged
            # copy. Identical contents are safe; different binaries are not.
            file(SHA256 "${_source_${_key}}" _previous_hash)
            file(SHA256 "${_source}" _source_hash)
            if(NOT _previous_hash STREQUAL _source_hash)
                message(FATAL_ERROR "Conflicting networking runtime basename ${_name} for configuration ${NETWORK_CONFIG}:\n  ${_source_${_key}}\n  ${_source}\nSelect one DLL per basename in this configuration.")
            endif()
        endif()
        set(_source_${_key} "${_source}")
        set(_source_key_${_key} "${_source_key}")
    endforeach()
endfunction()
# Reject supplemental/target conflicts before scanning or copying any DLLs.
luminumbra_reject_network_basename_conflicts(${_runtime_dlls})
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
set(_executable_args "")
if(NETWORK_EXECUTABLE)
    set(_executable_args EXECUTABLES "${NETWORK_EXECUTABLE}")
endif()
if(_runtime_dlls OR NETWORK_EXECUTABLE)
    file(GET_RUNTIME_DEPENDENCIES
        ${_executable_args}
        LIBRARIES ${_runtime_dlls}
        DIRECTORIES ${_search_dirs}
        RESOLVED_DEPENDENCIES_VAR _resolved
        UNRESOLVED_DEPENDENCIES_VAR _unresolved
        CONFLICTING_DEPENDENCIES_PREFIX _conflicts
        PRE_EXCLUDE_REGEXES "^api-ms-.*" "^ext-ms-.*"
        POST_EXCLUDE_REGEXES ${_system_regexes})
endif()
if(_conflicts_FILENAMES)
    foreach(_name IN LISTS _conflicts_FILENAMES)
        list(APPEND _resolved ${_conflicts_${_name}})
    endforeach()
endif()
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
luminumbra_reject_network_basename_conflicts(${_runtime_dlls})
if(NETWORK_EXECUTABLE)
    foreach(_dll IN LISTS _runtime_dlls)
        get_filename_component(_name "${_dll}" NAME)
        set(_staged "${NETWORK_RUNTIME_DIR}/${_name}")
        if(NOT EXISTS "${_staged}")
            message(FATAL_ERROR "Networking runtime ${_name} imported by ${NETWORK_EXECUTABLE} was not staged. Add its DLL to the shared networking runtime inputs.")
        endif()
        file(SHA256 "${_dll}" _source_hash)
        file(SHA256 "${_staged}" _staged_hash)
        if(NOT _source_hash STREQUAL _staged_hash)
            message(FATAL_ERROR "Staged networking runtime ${_name} differs from ${_dll} required by ${NETWORK_EXECUTABLE}")
        endif()
    endforeach()
else()
    file(MAKE_DIRECTORY "${NETWORK_RUNTIME_DIR}")
    if(_runtime_dlls)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_runtime_dlls} "${NETWORK_RUNTIME_DIR}"
            RESULT_VARIABLE _copy_result)
        if(NOT _copy_result EQUAL 0)
            message(FATAL_ERROR "Failed to copy shared networking runtime DLLs into ${NETWORK_RUNTIME_DIR}")
        endif()
    endif()
    get_filename_component(_stamp_dir "${NETWORK_STAMP}" DIRECTORY)
    file(MAKE_DIRECTORY "${_stamp_dir}")
    file(TOUCH "${NETWORK_STAMP}")
endif()
