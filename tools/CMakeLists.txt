# tools/CMakeLists.txt

add_executable(asset_processor
    asset_processor.cpp
)

# Tell the target where to find "meshoptimizer.h"
target_include_directories(asset_processor PUBLIC
    # For luminumbra_common/animation/SkinnedMeshFormat.h (LMS2/.lanim formats)
    ${CMAKE_SOURCE_DIR}/src
)
# Vendor headers are SYSTEM includes: third-party code (stb, meshoptimizer)
# is not held to the project's -Werror warning policy.
target_include_directories(asset_processor SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src
    ${CMAKE_SOURCE_DIR}/vendor/stb
)

# Add the required meshoptimizer .cpp files to be compiled with your tool.
# We only need to add the files for the functions we actually use.
target_sources(asset_processor PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/clusterizer.cpp      # For meshopt_computeMeshBounds
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/indexgenerator.cpp   # For meshopt_generateVertexRemap, etc.
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/overdrawoptimizer.cpp  # For meshopt_optimizeOverdraw
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/vcacheoptimizer.cpp    # For meshopt_optimizeVertexCache
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/vfetchoptimizer.cpp    # For meshopt_optimizeVertexFetch
    ${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/vertexfilter.cpp
)