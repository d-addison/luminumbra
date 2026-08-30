# Third-Party Licenses

Luminumbra is distributed under the MIT License (see `LICENSE`). It builds on the
third-party projects below, each under its own license. All are permissive and
compatible with MIT redistribution.

Dependencies are obtained in one of two ways, noted per entry:

- **vendored** — source is committed in this repository under `vendor/`
- **fetched** — downloaded at configure time by CMake `FetchContent`

---

## Vendored

| Project | Use | License |
| --- | --- | --- |
| [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) | Coherent noise for terrain generation | MIT |
| [RenderDoc](https://github.com/baldurk/renderdoc) (`renderdoc_app.h` only) | In-application graphics capture API | MIT |

## Fetched at configure time

| Project | Use | License |
| --- | --- | --- |
| [EnTT](https://github.com/skypjack/entt) | Entity-component-system registry | MIT |
| [GLM](https://github.com/g-truc/glm) | Vector and matrix math | MIT |
| [GLFW](https://github.com/glfw/glfw) | Window creation and input | Zlib |
| [glad](https://github.com/Dav1dde/glad) | OpenGL loader (generated) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing and serialization | MIT |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Rigid-body physics and collision | MIT |
| [FreeType](https://github.com/freetype/freetype) | Font rasterization for RmlUi | FreeType License or GPL-2.0-or-later |
| [RmlUi](https://github.com/mikke89/RmlUi) | Runtime user interface | MIT |
| [SOIL2](https://github.com/SpartanJ/SOIL2) | Image loading and OpenGL textures | Public domain |
| [miniaudio](https://github.com/mackron/miniaudio) | Audio playback and mixing | MIT or public domain |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Mesh optimization | MIT |
| [Lua](https://github.com/lua/lua) | Runtime scripting language | MIT |
| [sol2](https://github.com/ThePhD/sol2) | C++ binding layer for Lua | MIT |
| [spdlog](https://github.com/gabime/spdlog) | Logging | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | Immediate-mode debug UI | MIT |
| [stb](https://github.com/nothings/stb) | Image loading and utility headers | MIT or public domain |
| [GoogleTest](https://github.com/google/googletest) | Unit and integration test framework | BSD-3-Clause |
| [LZ4](https://github.com/lz4/lz4) | Fast compression | BSD-2-Clause |

---

## Notes

**Fetched dependencies are not redistributed by this repository.** CMake downloads
them into the build tree at configure time; their source is not committed here and
is not covered by this repository's license.

**The Steamworks SDK is not included.** It is Valve proprietary and may not be
redistributed. Any local `vendor/steamworks` directory is untracked by design and
must never be committed. Building the Steam integration requires obtaining the SDK
directly from Valve under their own agreement.

Full license texts ship with each dependency in its own source tree. Where a
project's license requires that its copyright notice accompany redistribution,
that notice is preserved in the vendored source rather than reproduced here.
