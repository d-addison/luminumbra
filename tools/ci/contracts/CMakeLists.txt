cmake_minimum_required(VERSION 3.20)
project(LocalCheckContracts LANGUAGES NONE)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/../local_check_contracts.cmake")
