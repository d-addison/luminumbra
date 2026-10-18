# Apply the reviewed edits into build-owned files. Supplied FetchContent sources,
# including offline and shared overrides, remain read-only.
function(luminumbra_prepare_jolt_startup SOURCE_DIR OUTPUT_DIR)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
  set(_manifest "${_root}/cmake/patches/jolt-startup.json")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I -B "${_root}/tools/ci/jolt_startup.py"
      --source "${SOURCE_DIR}" --output "${OUTPUT_DIR}"
    RESULT_VARIABLE _result ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Jolt startup patch failed: ${_error}")
  endif()
  file(READ "${_manifest}" _edits)
  string(JSON _count LENGTH "${_edits}" files)
  math(EXPR _last "${_count} - 1")
  get_target_property(_sources Jolt SOURCES)
  foreach(_index RANGE 0 ${_last})
    string(JSON _path GET "${_edits}" files ${_index} path)
    set(_original "${SOURCE_DIR}/${_path}")
    set(_patched "${OUTPUT_DIR}/${_path}")
    set(_source_index -1)
    set(_candidate_index 0)
    foreach(_candidate IN LISTS _sources)
      get_filename_component(_absolute "${_candidate}" ABSOLUTE BASE_DIR "${SOURCE_DIR}/Build")
      if(_absolute STREQUAL _original)
        if(NOT _source_index EQUAL -1)
          message(FATAL_ERROR "Duplicate Jolt input ${_path}")
        endif()
        set(_source_index ${_candidate_index})
      endif()
      math(EXPR _candidate_index "${_candidate_index} + 1")
    endforeach()
    if(_source_index EQUAL -1)
      message(FATAL_ERROR "Jolt target is missing reviewed input ${_path}")
    endif()
    list(REMOVE_AT _sources ${_source_index})
    list(APPEND _sources "${_patched}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_original}")
  endforeach()
  set_property(TARGET Jolt PROPERTY SOURCES "${_sources}")
  # Headers and source TUs share the same overlay, including every consumer and
  # the existing precompiled header. All upstream flags and capacities persist.
  target_include_directories(Jolt BEFORE PUBLIC "$<BUILD_INTERFACE:${OUTPUT_DIR}>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_manifest}" "${_root}/cmake/patches/jolt-startup.patch"
    "${_root}/tools/ci/jolt_startup.py" "${_root}/tools/ci/path_safety.py")
endfunction()
