# RmlUi is delivered only into a build root enrolled before configure.
# Use the bootstrap caller's interpreter, including its native path spelling.
# An empty binding must refuse instead of falling back to independent discovery.
if(NOT IS_ABSOLUTE "${Python3_EXECUTABLE}" OR NOT EXISTS "${Python3_EXECUTABLE}")
  message(FATAL_ERROR "RmlUi requires an absolute bootstrap Python3_EXECUTABLE")
endif()
file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" _rmlui_python)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_rmlui_helper "${CMAKE_SOURCE_DIR}/tools/ci/rmlui_source.py")
set(_rmlui_manifest "${CMAKE_SOURCE_DIR}/vendor/rmlui/rmlui-6.1-source.json")
set(_rmlui_patch "${CMAKE_SOURCE_DIR}/cmake/patches/rmlui-6.1-shadow.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_rmlui_helper}" "${_rmlui_manifest}" "${_rmlui_patch}")
set(_rmlui_identity "60304a1ae56d9a21c2e0db0853d02b5946bc4ac2030aa8f21ef84d9ae752f1de")
set(_rmlui_expected_source "${CMAKE_BINARY_DIR}/_deps/rmlui-${_rmlui_identity}-src")
# FetchContent creates SOURCE_DIR before DOWNLOAD_COMMAND. Keep that empty
# population directory separate from the source published by the helper.
set(_rmlui_population "${CMAKE_BINARY_DIR}/_deps/rmlui-${_rmlui_identity}-population")
set(_rmlui_expected_binary "${CMAKE_BINARY_DIR}/_deps/rmlui-${_rmlui_identity}-build")
get_property(_rmlui_declared GLOBAL PROPERTY _FetchContent_rmlui_savedDetails SET)
get_property(_rmlui_provider GLOBAL PROPERTY __FETCHCONTENT_MAKEAVAILABLE_SERIAL_PROVIDER SET)
FetchContent_GetProperties(RmlUi)
if(_rmlui_declared OR _rmlui_provider OR rmlui_POPULATED OR
   NOT "${CMAKE_PROJECT_TOP_LEVEL_INCLUDES}" STREQUAL "" OR
   NOT "${FETCHCONTENT_SOURCE_DIR_RMLUI}" STREQUAL "" OR
   FETCHCONTENT_TRY_FIND_PACKAGE_MODE STREQUAL "ALWAYS")
  message(FATAL_ERROR "RmlUi requires the owned source delivery; overrides and providers are ineligible")
endif()
get_filename_component(_rmlui_base "${FETCHCONTENT_BASE_DIR}" ABSOLUTE)
if(NOT _rmlui_base STREQUAL "${CMAKE_BINARY_DIR}/_deps")
  message(FATAL_ERROR "RmlUi requires the build-local FetchContent base directory")
endif()
if(EXISTS "${_rmlui_population}" AND NOT EXISTS "${CMAKE_BINARY_DIR}/.rmlui-ready.json")
  message(FATAL_ERROR "RmlUi population is incomplete; initialize a fresh build root")
endif()
set(_rmlui_disconnected)
if(FETCHCONTENT_FULLY_DISCONNECTED)
  list(APPEND _rmlui_disconnected --fully-disconnected)
endif()
execute_process(COMMAND "${_rmlui_python}" -I -B "${_rmlui_helper}"
  preflight --root "${CMAKE_BINARY_DIR}" ${_rmlui_disconnected}
  RESULT_VARIABLE _rmlui_preflight)
if(NOT _rmlui_preflight EQUAL 0)
  message(FATAL_ERROR "RmlUi ownership preflight failed; initialize a fresh build root before configure")
endif()
FetchContent_Declare(RmlUi
  SOURCE_DIR "${_rmlui_population}"
  BINARY_DIR "${_rmlui_expected_binary}"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  SOURCE_SUBDIR "rmlui-population-only-${_rmlui_identity}")
FetchContent_MakeAvailable(RmlUi)
if(NOT rmlui_SOURCE_DIR STREQUAL _rmlui_population OR
   NOT rmlui_BINARY_DIR STREQUAL _rmlui_expected_binary)
  message(FATAL_ERROR "RmlUi population substituted an unexpected directory")
endif()
# Keep the exact bootstrap interpreter out of generated CMake source strings.
# Population remains owned and empty; delivery still precedes verification and
# upstream configuration, including when fully disconnected.
execute_process(COMMAND "${_rmlui_python}" -I -B "${_rmlui_helper}"
  download --root "${CMAKE_BINARY_DIR}" ${_rmlui_disconnected}
  RESULT_VARIABLE _rmlui_downloaded)
if(NOT _rmlui_downloaded EQUAL 0)
  message(FATAL_ERROR "RmlUi source delivery failed before upstream configuration")
endif()
execute_process(COMMAND "${_rmlui_python}" -I -B "${_rmlui_helper}"
  verify --root "${CMAKE_BINARY_DIR}"
  RESULT_VARIABLE _rmlui_verified)
if(NOT _rmlui_verified EQUAL 0)
  message(FATAL_ERROR "RmlUi source verification failed before upstream configuration")
endif()
set(rmlui_SOURCE_DIR "${_rmlui_expected_source}")
add_subdirectory("${rmlui_SOURCE_DIR}" "${rmlui_BINARY_DIR}")
