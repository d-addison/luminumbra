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
target_link_libraries(asset_processor PRIVATE meshoptimizer stb_headers)
