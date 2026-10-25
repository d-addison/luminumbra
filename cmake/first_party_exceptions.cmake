# CMAKE_CXX_FLAGS is caller-owned (for example, symbol collection replaces it
# with /Z7). Do not rely on CMake's default /EHsc surviving that replacement.
# Apply the C++ unwind contract to compiled first-party targets independently of
# warning policy: client libraries and some tests do not use the warning helper.
function(luminumbra_enable_directory_exceptions SOURCE_DIRECTORY)
  get_property(_exception_targets DIRECTORY "${SOURCE_DIRECTORY}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_exception_target IN LISTS _exception_targets)
    get_target_property(_exception_type ${_exception_target} TYPE)
    if(_exception_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
      target_compile_options(${_exception_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
    endif()
  endforeach()

  get_property(_exception_children DIRECTORY "${SOURCE_DIRECTORY}" PROPERTY SUBDIRECTORIES)
  foreach(_exception_child IN LISTS _exception_children)
    # FetchContent can add external source directories below a first-party
    # directory's configure scope. Keep dependency targets outside this policy.
    file(RELATIVE_PATH _exception_relative "${SOURCE_DIRECTORY}" "${_exception_child}")
    if(NOT IS_ABSOLUTE "${_exception_relative}" AND
       NOT _exception_relative MATCHES "^[.][.](/|$)")
      luminumbra_enable_directory_exceptions("${_exception_child}")
    endif()
  endforeach()
endfunction()

if(MSVC)
  get_property(_exception_root_children DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
  foreach(_exception_subtree IN ITEMS src tools test)
    set(_exception_directory "${CMAKE_CURRENT_SOURCE_DIR}/${_exception_subtree}")
    if(_exception_directory IN_LIST _exception_root_children)
      luminumbra_enable_directory_exceptions("${_exception_directory}")
    endif()
  endforeach()
endif()
