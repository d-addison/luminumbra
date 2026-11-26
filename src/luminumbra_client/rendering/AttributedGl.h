#pragma once

#include "../app/ProgramFirstUse.h"

namespace Luminumbra::Rendering::AttributedGl {

// Both hosts support one GL context on its owning render thread. In particular,
// do not make this TLS: disabled attribution must not initialize/access TLS on
// every GL bind or submission. Creation/deletion also run on the context owner.
namespace detail {
struct ProgramStorage {
    mutable Client::Attribution::ProgramFirstUse state;
};
// Constant initialization precedes dynamic initialization in every TU, including
// a wrapper called by a static initializer. The const owner retains destruction
// of the lazily created registry; only its render-thread-owned state is mutable.
inline constinit const ProgramStorage program_storage{};
} // namespace detail

inline Client::Attribution::ProgramFirstUse& programs() {
    return detail::program_storage.state;
}
using Program = Client::Attribution::ProgramFirstUse::Program;

// Keep the GL entry points typed and supplied by the caller's loader. The UI
// backend also supports GLES and its own loader on Windows and other platforms.
template<typename Function>
inline Program CreateProgram(Function function) {
    return programs().created(function());
}
template<typename Function>
inline void DeleteProgram(Function function, std::uint32_t program) {
    programs().deleted(program);
    function(program);
}
template<typename Function>
inline void UseProgram(Function function, const Program& program) {
    programs().use(function, program);
}
template<typename Function>
inline void UseProgram(Function function, std::uint32_t program) {
    programs().use(function, program);
}
template<typename Function, typename... Args>
inline void Submit(Function function, Args&&... args) {
    programs().submit(function, std::forward<Args>(args)...);
}
} // namespace Luminumbra::Rendering::AttributedGl
