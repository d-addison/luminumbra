if(NOT CMAKE_CXX_COMPILER_ID OR NOT CMAKE_CXX_COMPILER_VERSION)
  return()
endif()

set(_luminumbra_compiler_target "${CMAKE_CXX_COMPILER_TARGET}")
if(NOT _luminumbra_compiler_target)
  set(_luminumbra_compiler_target "${CMAKE_SYSTEM_PROCESSOR}")
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" _luminumbra_build_type_upper)
set(_luminumbra_effective_flags
  "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_luminumbra_build_type_upper}}")
string(SHA256 _luminumbra_flags_sha256 "${_luminumbra_effective_flags}")

foreach(_luminumbra_option IN ITEMS
    LUMINUMBRA_ENABLE_ASAN
    LUMINUMBRA_ENABLE_COVERAGE
    LUMINUMBRA_ENABLE_DILIGENT
    LUMINUMBRA_ENABLE_FRAME_POINTERS
    LUMINUMBRA_ENABLE_TRACY
    LUMINUMBRA_WARNINGS_AS_ERRORS)
  if(NOT DEFINED ${_luminumbra_option})
    set(${_luminumbra_option} OFF)
  endif()
endforeach()

file(WRITE "${CMAKE_BINARY_DIR}/performance-build.json"
  "{\n"
  "  \"schema\": \"luminumbra.performance_build.v1\",\n"
  "  \"compiler\": {\n"
  "    \"id\": \"${CMAKE_CXX_COMPILER_ID}\",\n"
  "    \"version\": \"${CMAKE_CXX_COMPILER_VERSION}\",\n"
  "    \"target\": \"${_luminumbra_compiler_target}\"\n"
  "  },\n"
  "  \"configuration\": {\n"
  "    \"build_type\": \"${CMAKE_BUILD_TYPE}\",\n"
  "    \"effective_flags_sha256\": \"${_luminumbra_flags_sha256}\",\n"
  "    \"asan\": \"${LUMINUMBRA_ENABLE_ASAN}\",\n"
  "    \"coverage\": \"${LUMINUMBRA_ENABLE_COVERAGE}\",\n"
  "    \"diligent\": \"${LUMINUMBRA_ENABLE_DILIGENT}\",\n"
  "    \"frame_pointers\": \"${LUMINUMBRA_ENABLE_FRAME_POINTERS}\",\n"
  "    \"tracy\": \"${LUMINUMBRA_ENABLE_TRACY}\",\n"
  "    \"warnings_as_errors\": \"${LUMINUMBRA_WARNINGS_AS_ERRORS}\"\n"
  "  },\n"
  "  \"system\": {\n"
  "    \"name\": \"${CMAKE_SYSTEM_NAME}\",\n"
  "    \"processor\": \"${CMAKE_SYSTEM_PROCESSOR}\"\n"
  "  }\n"
  "}\n")
