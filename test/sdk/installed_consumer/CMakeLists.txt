cmake_minimum_required(VERSION 3.20)
project(LuminumbraContractConsumer LANGUAGES C CXX)

set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)
find_package(Luminumbra CONFIG REQUIRED COMPONENTS ABI ModuleContracts)

add_library(abi_consumer OBJECT abi.c)
target_link_libraries(abi_consumer PRIVATE Luminumbra::ABI)
set_target_properties(abi_consumer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
add_executable(installed_consumer main.cpp $<TARGET_OBJECTS:abi_consumer>)
target_link_libraries(installed_consumer PRIVATE Luminumbra::ModuleContracts)
enable_testing()
add_test(NAME InstalledContractConsumer COMMAND installed_consumer)
