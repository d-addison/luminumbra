add_library(luminumbra_common STATIC "")

# Automated sources
include(sources.cmake)
target_sources(luminumbra_common PRIVATE ${COMMON_SOURCES})

# --- CORRECTED USAGE REQUIREMENTS ---
target_include_directories(luminumbra_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(luminumbra_common PUBLIC EnTT::EnTT)