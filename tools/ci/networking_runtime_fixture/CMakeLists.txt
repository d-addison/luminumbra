cmake_minimum_required(VERSION 3.21)

# Initialize the platform directory before installing the outer compiler's
# identity, ABI, features and tools. Enabling CXX then uses CMake's configured
# compiler path, avoiding repeated compiler and binutils discovery. Language
# rules still initialize; the fixture must still compile, link, stage and run.
project(NetworkingRuntimeFixture LANGUAGES NONE)
function(reuse_outer_compiler)
    # Inspect in a function scope so enable_language loads the installed file.
    include("${NETWORK_TEST_COMPILER_INFO}" OPTIONAL RESULT_VARIABLE _compiler_info)
    if(NOT _compiler_info OR NOT CMAKE_CXX_COMPILER OR NOT CMAKE_CXX_COMPILER_ID
       OR NOT CMAKE_CXX_COMPILER_LOADED OR NOT CMAKE_CXX_COMPILER_ID_RUN OR NOT CMAKE_CXX_COMPILER_WORKS
       OR NOT CMAKE_CXX_ABI_COMPILED)
        message(FATAL_ERROR "Outer CXX compiler information is incomplete")
    endif()
    configure_file("${NETWORK_TEST_COMPILER_INFO}"
        "${CMAKE_PLATFORM_INFO_DIR}/CMakeCXXCompiler.cmake" COPYONLY)
endfunction()
reuse_outer_compiler()
enable_language(CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(LUMINUMBRA_ENABLE_GNS ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/networking_runtime.cmake")

add_library(network_leaf SHARED leaf.cpp)
add_library(network_backend SHARED backend.cpp)
foreach(_library IN ITEMS network_leaf network_backend)
    set_target_properties(${_library} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package files/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package files/$<CONFIG>"
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
endforeach()
target_compile_definitions(network_leaf PRIVATE "FIXTURE_VALUE=$<IF:$<CONFIG:Debug>,41,42>")
# Link by file so TARGET_RUNTIME_DLLS cannot supply this transitive DLL. The
# real PE scanner must find it beside network_backend in the package directory.
target_link_libraries(network_backend PRIVATE "$<TARGET_LINKER_FILE:network_leaf>")
add_dependencies(network_backend network_leaf)

add_executable(network_preview main.cpp)
target_link_libraries(network_preview PRIVATE network_backend)
target_compile_definitions(network_preview PRIVATE "FIXTURE_VALUE=$<IF:$<CONFIG:Debug>,41,42>")

# Exercise configuration-specific supplemental inputs as well as target DLLs,
# without fetching any networking packages or linking the engine.
add_library(luminumbra_common INTERFACE)
luminumbra_network_configurations(_configs)
foreach(_config IN LISTS _configs)
    string(TOUPPER "${_config}" _key)
    set(_package "${CMAKE_CURRENT_BINARY_DIR}/package files/${_config}")
    set(_backend "${_package}/${CMAKE_SHARED_LIBRARY_PREFIX}network_backend${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set_target_properties(luminumbra_common PROPERTIES
        LUMINUMBRA_NETWORK_RUNTIME_DIRS_${_key} "${_package}"
        LUMINUMBRA_NETWORK_PACKAGE_DLLS_${_key} "${_backend}"
        LUMINUMBRA_GNS_RUNTIME_DLLS_${_key} "${_backend}"
        LUMINUMBRA_NETWORK_OPENSSL_DIRS_${_key} "")
endforeach()
luminumbra_stage_networking_runtime(network_preview)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/manifest-$<CONFIG>.cmake" CONTENT
"set(FIXTURE_EXECUTABLE [==[$<TARGET_FILE:network_preview>]==])
set(FIXTURE_RUNTIME_DIR [==[$<TARGET_FILE_DIR:network_preview>]==])
set(FIXTURE_BACKEND [==[$<TARGET_FILE:network_backend>]==])
set(FIXTURE_LEAF [==[$<TARGET_FILE:network_leaf>]==])
set(FIXTURE_TARGET_DLLS [==[$<TARGET_RUNTIME_DLLS:network_preview>]==])
")
