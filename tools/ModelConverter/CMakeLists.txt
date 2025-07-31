# Set the minimum required version of CMake
cmake_minimum_required(VERSION 3.10)

# Set the project name
project(ModelConverter)

# Find the Assimp library package. 
# This is crucial and requires Assimp to be installed or located by CMake.
find_package(assimp REQUIRED)

# Add the executable target, specifying its name and source file
add_executable(model_converter main.cpp)

# Link the executable against the Assimp library
target_link_libraries(model_converter PRIVATE assimp::assimp)