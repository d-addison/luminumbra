cmake_minimum_required(VERSION 3.20)

get_filename_component(_modules "${CMAKE_CURRENT_LIST_DIR}/../../cmake" ABSOLUTE)

if(NETWORK_TEST_MODE STREQUAL "version")
    # Simulate the configure variables without a compiler or backend packages.
    set(CMAKE_VERSION "${TEST_CMAKE_VERSION}")
    include("${_modules}/networking_runtime.cmake")
    return()
elseif(NETWORK_TEST_MODE STREQUAL "runtime")
    # Only the PE import scanner is fake. All other file operations, copying,
    # conflict checks and post-build verification run the production helper.
    macro(file OPERATION)
        if("${OPERATION}" STREQUAL "GET_RUNTIME_DEPENDENCIES")
            if(TEST_EXPECT_NO_SCAN)
                message(FATAL_ERROR "Empty staging inputs must not invoke the import scanner")
            endif()
            cmake_parse_arguments(_scan ""
                "RESOLVED_DEPENDENCIES_VAR;UNRESOLVED_DEPENDENCIES_VAR;CONFLICTING_DEPENDENCIES_PREFIX"
                "EXECUTABLES;LIBRARIES;DIRECTORIES;PRE_EXCLUDE_REGEXES;POST_EXCLUDE_REGEXES" ${ARGN})
            if(NOT "${_scan_LIBRARIES}" STREQUAL "${TEST_EXPECT_LIBRARIES}"
               OR NOT "${_scan_EXECUTABLES}" STREQUAL "${NETWORK_EXECUTABLE}")
                message(FATAL_ERROR "Import scanner received incorrect binaries")
            endif()
            foreach(_scan_dll IN LISTS _scan_LIBRARIES)
                get_filename_component(_scan_bin "${_scan_dll}" DIRECTORY)
                if(NOT _scan_bin IN_LIST _scan_DIRECTORIES)
                    message(FATAL_ERROR "Import scanner omitted input directory ${_scan_bin}")
                endif()
            endforeach()
            _file(STRINGS "${TEST_IMPORT_LIST}" _imports)
            set(${_scan_RESOLVED_DEPENDENCIES_VAR} "${_imports}")
            set(${_scan_UNRESOLVED_DEPENDENCIES_VAR} "${TEST_UNRESOLVED}")
            set(${_scan_CONFLICTING_DEPENDENCIES_PREFIX}_FILENAMES "")
            set(TEST_SCAN_CALLED TRUE)
        else()
            _file(${ARGV})
        endif()
    endmacro()
    include("${_modules}/copy_networking_runtime.cmake")
    if(NOT TEST_EXPECT_NO_SCAN AND NOT TEST_SCAN_CALLED)
        message(FATAL_ERROR "Import scanner was not invoked")
    endif()
    return()
endif()

if(NOT NETWORK_TEST_ROOT)
    message(FATAL_ERROR "Pass NETWORK_TEST_ROOT for isolated test fixtures")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run)
set(_root "${NETWORK_TEST_ROOT}/run-${_run}")
file(MAKE_DIRECTORY "${_root}")

function(run_child NAME EXPECTED_ERROR)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    set(_output "${_stdout}\n${_stderr}")
    if(EXPECTED_ERROR)
        if("${_result}" STREQUAL "0" OR NOT _output MATCHES "${EXPECTED_ERROR}")
            message(FATAL_ERROR "${NAME}: expected refusal '${EXPECTED_ERROR}', got ${_result}:\n${_output}")
        endif()
    elseif(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${NAME}: failed (${_result}):\n${_output}")
    endif()
endfunction()

function(assert_contents PATH EXPECTED)
    if(NOT EXISTS "${PATH}")
        message(FATAL_ERROR "Missing fixture output: ${PATH}")
    endif()
    file(READ "${PATH}" _actual)
    if(NOT _actual STREQUAL EXPECTED)
        message(FATAL_ERROR "Incorrect contents at ${PATH}: '${_actual}'")
    endif()
endfunction()

foreach(_backend IN ITEMS GNS STEAM)
    run_child("Windows ${_backend} with CMake 3.20" "require CMake 3.21[+]"
        -DNETWORK_TEST_MODE=version -DWIN32=TRUE -DTEST_CMAKE_VERSION=3.20.6
        "-DLUMINUMBRA_ENABLE_${_backend}=ON")
    run_child("Windows ${_backend} with CMake 3.21" ""
        -DNETWORK_TEST_MODE=version -DWIN32=TRUE -DTEST_CMAKE_VERSION=3.21.0
        "-DLUMINUMBRA_ENABLE_${_backend}=ON")
    run_child("Linux ${_backend} with CMake 3.20" ""
        -DNETWORK_TEST_MODE=version -DWIN32=FALSE -DTEST_CMAKE_VERSION=3.20.6
        "-DLUMINUMBRA_ENABLE_${_backend}=ON")
endforeach()
run_child("Windows without networking with CMake 3.20" ""
    -DNETWORK_TEST_MODE=version -DWIN32=TRUE -DTEST_CMAKE_VERSION=3.20.6)

include("${_modules}/networking_runtime.cmake")
set(_compiler_bin "${_root}/compiler bin")
file(MAKE_DIRECTORY "${_compiler_bin}")
foreach(_name IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll libc++.dll libunwind.dll)
    file(WRITE "${_compiler_bin}/${_name}" "${_name}")
endforeach()
foreach(CMAKE_CXX_COMPILER_ID IN ITEMS GNU Clang)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_expected_names libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    else()
        set(_expected_names libc++.dll libunwind.dll libwinpthread-1.dll)
    endif()
    set(_expected "")
    foreach(_name IN LISTS _expected_names)
        list(APPEND _expected "${_compiler_bin}/${_name}")
    endforeach()
    luminumbra_network_mingw_compiler_dlls(_actual "${_compiler_bin}")
    if(NOT "${_actual}" STREQUAL "${_expected}")
        message(FATAL_ERROR "Incorrect ${CMAKE_CXX_COMPILER_ID} runtime selection: ${_actual}")
    endif()
endforeach()
set(CMAKE_CXX_COMPILER_ID Clang)
file(REMOVE "${_compiler_bin}/libgcc_s_seh-1.dll" "${_compiler_bin}/libstdc++-6.dll"
    "${_compiler_bin}/libwinpthread-1.dll")
luminumbra_network_mingw_compiler_dlls(_actual "${_compiler_bin}")
if(NOT "${_actual}" STREQUAL "${_compiler_bin}/libc++.dll;${_compiler_bin}/libunwind.dll")
    message(FATAL_ERROR "Clang must work without GCC DLLs or optional winpthreads")
endif()
file(REMOVE_RECURSE "${_compiler_bin}/libunwind.dll")
file(MAKE_DIRECTORY "${_compiler_bin}/libunwind.dll")
luminumbra_network_mingw_compiler_dlls(_actual "${_compiler_bin}")
if(NOT "${_actual}" STREQUAL "${_compiler_bin}/libc++.dll")
    message(FATAL_ERROR "A directory must not be selected as a runtime DLL")
endif()
set(CMAKE_CXX_COMPILER_ID GNU)
luminumbra_network_mingw_compiler_dlls(_actual "${_compiler_bin}")
if(_actual)
    message(FATAL_ERROR "Unimported GCC runtimes need not be installed")
endif()

foreach(_config IN ITEMS Debug Release)
    # Equal basenames with different contents catch accidental cross-config
    # copies; spaces exercise generated input-list paths and subprocess quoting.
    set(_source "${_root}/package files/${_config}")
    set(_runtime "${_root}/bin/${_config}")
    set(_state "${_root}/network-runtime/${_config}")
    file(MAKE_DIRECTORY "${_source}" "${_state}")
    file(WRITE "${_source}/backend.dll" "backend ${_config}")
    file(WRITE "${_source}/dependency.dll" "dependency ${_config}")
    file(WRITE "${_state}/inputs.txt" "\n${_source}/backend.dll\n${_source}/backend.dll\n\n")
    file(WRITE "${_state}/imports.txt" "${_source}/dependency.dll\n")
    file(WRITE "${_state}/preview.exe" "fake executable")
    set(_args -DNETWORK_TEST_MODE=runtime
        "-DNETWORK_CONFIG=${_config}"
        "-DNETWORK_INPUT_FILE=${_state}/inputs.txt"
        "-DNETWORK_RUNTIME_DIR=${_runtime}"
        "-DNETWORK_STAMP=${_state}/staged.stamp"
        "-DTEST_EXPECT_LIBRARIES=${_source}/backend.dll"
        "-DTEST_IMPORT_LIST=${_state}/imports.txt")
    run_child("Stage ${_config}" "" ${_args})
    assert_contents("${_runtime}/backend.dll" "backend ${_config}")
    assert_contents("${_runtime}/dependency.dll" "dependency ${_config}")
    assert_contents("${_state}/staged.stamp" "")
    file(REMOVE "${_runtime}/dependency.dll")
    run_child("Repair deleted ${_config} DLL" "" ${_args})
    assert_contents("${_runtime}/dependency.dll" "dependency ${_config}")

    set(_verify ${_args} "-DNETWORK_EXECUTABLE=${_state}/preview.exe")
    run_child("Verify ${_config}" "" ${_verify})
    file(REMOVE "${_runtime}/dependency.dll")
    run_child("Refuse deleted ${_config} import" "dependency.dll.*was not staged" ${_verify})
    if(EXISTS "${_runtime}/dependency.dll")
        message(FATAL_ERROR "Verification must not repair missing DLLs")
    endif()
    run_child("Restore ${_config}" "" ${_args})
    # Model a direct executable import absent from the shared staging inputs.
    file(WRITE "${_source}/direct.dll" "direct ${_config}")
    file(APPEND "${_state}/imports.txt" "${_source}/direct.dll\n")
    run_child("Refuse unstaged ${_config} executable import" "direct.dll.*was not staged" ${_verify})
    if(EXISTS "${_runtime}/direct.dll")
        message(FATAL_ERROR "Verification copied an unstaged executable import")
    endif()
    file(WRITE "${_state}/imports.txt" "${_source}/dependency.dll\n")
    file(WRITE "${_runtime}/dependency.dll" "stale DLL")
    run_child("Refuse stale ${_config} DLL" "Staged networking runtime dependency.dll differs" ${_verify})
    run_child("Repair stale ${_config} DLL" "" ${_args})
    run_child("Refuse unresolved ${_config} import" "Unresolved non-system networking runtime imports"
        ${_verify} -DTEST_UNRESOLVED=missing.dll)
    foreach(_missing IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libc++.dll libunwind.dll libwinpthread-1.dll)
        run_child("Refuse missing imported ${_missing}" "Unresolved non-system networking runtime imports"
            ${_args} "-DTEST_UNRESOLVED=${_missing}")
    endforeach()
endforeach()
# Verify that running Release never overwrote the previously staged Debug DLLs.
assert_contents("${_root}/bin/Debug/backend.dll" "backend Debug")
assert_contents("${_root}/bin/Debug/dependency.dll" "dependency Debug")

foreach(_empty IN ITEMS zero-byte blank-lines)
    file(WRITE "${_root}/empty-inputs.txt" "")
    if(_empty STREQUAL "blank-lines")
        file(WRITE "${_root}/empty-inputs.txt" "\n\n")
    endif()
    run_child("Stage ${_empty} inputs" "" -DNETWORK_TEST_MODE=runtime
        "-DNETWORK_INPUT_FILE=${_root}/empty-inputs.txt"
        "-DNETWORK_RUNTIME_DIR=${_root}/bin/empty"
        "-DNETWORK_STAMP=${_root}/network-runtime/empty/staged.stamp"
        -DTEST_EXPECT_NO_SCAN=TRUE)
    assert_contents("${_root}/network-runtime/empty/staged.stamp" "")
    file(GLOB _empty_dlls "${_root}/bin/empty/*")
    if(NOT IS_DIRECTORY "${_root}/bin/empty" OR _empty_dlls)
        message(FATAL_ERROR "Empty staging must create an empty runtime directory")
    endif()
endforeach()

file(REMOVE_RECURSE "${_root}")
message(STATUS "Networking runtime contract passed")
