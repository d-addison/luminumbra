# cmake/dependencies.cmake
include(FetchContent)

# --- Find System-Provided Packages (installed with pacman) ---
# For a MinGW environment, this is the most reliable method.
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(Freetype REQUIRED)

# --- Helper Macro for remaining dependencies ---
macro(FetchDep name git_repo git_tag)
    FetchContent_Declare(
        ${name}
        GIT_REPOSITORY ${git_repo}
        GIT_TAG        ${git_tag}
    )
    FetchContent_MakeAvailable(${name})
    list(APPEND THIRD_PARTY_LIBS ${name})
endmacro()

# ==============================================================================
# --- Define Dependencies to Fetch from Source ---
# ==============================================================================

# -- Core & Simulation --
FetchDep(EnTT           https://github.com/skypjack/entt.git           v3.15.0)
FetchDep(nlohmann_json  https://github.com/nlohmann/json.git           v3.12.0)

# -- GLAD (OpenGL Loader) - This MUST be built from source --
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.8
)
set(GLAD_PROFILE "core" CACHE STRING "OpenGL profile")
set(GLAD_API "gl=4.5" CACHE STRING "OpenGL API version")
FetchContent_MakeAvailable(glad)

# -- UI (RmlUi is not in pacman, so we fetch it) --
# RmlUi requires some options to be set before making it available.
FetchContent_Declare(
    RmlUi
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG        6.1
)
set(BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# We still need to tell RmlUi's build not to look for Freetype,
# because we want to link the one we found with find_package.
set(RMLUI_FREETYPE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(RmlUi)
list(APPEND THIRD_PARTY_LIBS RmlUi)


# -- Audio --
FetchDep(miniaudio      https://github.com/mackron/miniaudio.git      master)

# -- Image Loading (for RmlUi) --
FetchDep(soil2          https://github.com/SpartanJ/SOIL2.git          master)