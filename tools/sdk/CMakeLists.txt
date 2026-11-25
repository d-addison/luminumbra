cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(Luminumbra VERSION 0.3.0 LANGUAGES NONE)
endif()

get_filename_component(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SDK_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
# Stage only the public contracts, including for build-tree users.
configure_file("${SDK_ROOT}/include/luminumbra/abi/luminumbra_abi.h"
    "${SDK_INCLUDE_DIR}/luminumbra/abi/luminumbra_abi.h" COPYONLY)
foreach(header IN ITEMS ModuleRegistry.h PhaseScheduler.h)
    configure_file("${SDK_ROOT}/src/luminumbra_common/core/${header}"
        "${SDK_INCLUDE_DIR}/luminumbra/modules/${header}" COPYONLY)
endforeach()

add_library(luminumbra_abi INTERFACE)
add_library(Luminumbra::ABI ALIAS luminumbra_abi)
set_target_properties(luminumbra_abi PROPERTIES EXPORT_NAME ABI)
target_include_directories(luminumbra_abi INTERFACE
    "$<BUILD_INTERFACE:${SDK_INCLUDE_DIR}>" "$<INSTALL_INTERFACE:include>")

# C++ declarations and inline contracts only; runtime implementations are not
# linked by this target. Native shared-library boundaries use the C ABI target.
add_library(luminumbra_module_contracts INTERFACE)
add_library(Luminumbra::ModuleContracts ALIAS luminumbra_module_contracts)
set_target_properties(luminumbra_module_contracts PROPERTIES EXPORT_NAME ModuleContracts)
target_link_libraries(luminumbra_module_contracts INTERFACE luminumbra_abi)
target_compile_features(luminumbra_module_contracts INTERFACE cxx_std_20)

install(TARGETS luminumbra_abi luminumbra_module_contracts EXPORT LuminumbraTargets
    COMPONENT SDK)
install(EXPORT LuminumbraTargets NAMESPACE Luminumbra::
    DESTINATION lib/cmake/Luminumbra COMPONENT SDK)
install(FILES "${SDK_INCLUDE_DIR}/luminumbra/abi/luminumbra_abi.h"
    DESTINATION include/luminumbra/abi COMPONENT SDK)
install(FILES "${SDK_INCLUDE_DIR}/luminumbra/modules/ModuleRegistry.h"
              "${SDK_INCLUDE_DIR}/luminumbra/modules/PhaseScheduler.h"
    DESTINATION include/luminumbra/modules COMPONENT SDK)

include(CMakePackageConfigHelpers)
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/LuminumbraConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LuminumbraConfig.cmake"
    INSTALL_DESTINATION lib/cmake/Luminumbra)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/LuminumbraConfigVersion.cmake"
    VERSION "${Luminumbra_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LuminumbraConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/LuminumbraConfigVersion.cmake"
    DESTINATION lib/cmake/Luminumbra COMPONENT SDK)
