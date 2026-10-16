#pragma once

#include "FrameAttribution.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

#if defined(_MSC_VER)
#define LUMINUMBRA_FIRST_USE_COLD __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define LUMINUMBRA_FIRST_USE_COLD __attribute__((cold, noinline))
#else
#define LUMINUMBRA_FIRST_USE_COLD
#endif

namespace Luminumbra::Client::Attribution {

// One render context on the owning render thread. Storage is bounded by the peak
// number of live programs; deletion removes failed/unused programs as well.
// Binding alone does not consume first use: driver specialization can happen
// at the first draw/dispatch, including indirect and instanced submissions.
class ProgramFirstUse {
public:
    ProgramFirstUse() = default;
    ProgramFirstUse(const ProgramFirstUse&) = delete;
    ProgramFirstUse& operator=(const ProgramFirstUse&) = delete;
    struct State {
        bool pending = true;
    };
    inline static constinit State no_program{.pending = false};
    // A GL name plus its stable first-use bit. Like the GL name, this borrowed
    // handle is valid until DeleteProgram. Registry rehashes do not move State.
    struct Program {
        std::uint32_t name = 0;
        State* state = &no_program;
        operator std::uint32_t() const {
            return name;
        }
    };
    Program created(std::uint32_t program) {
        if (program == 0)
            return {};
        const auto entry = m_programs.insert_or_assign(program, State{}).first;
        return {.name = program, .state = &entry->second};
    }
    void deleted(std::uint32_t program) {
        const auto entry = m_programs.find(program);
        if (entry == m_programs.end())
            return;
        if (m_bound == &entry->second)
            m_bound = &no_program;
        m_programs.erase(entry);
    }
    void bind(const Program& program) {
        m_bound = program.state;
    }
    void bind(std::uint32_t program) {
        bind(lookup(program));
    }
    bool pending() const {
        return m_bound->pending;
    }
    // Const runtime diagnostic: calls that entered first-use phase attribution.
    // Unlike completed phase totals, this also detects an inactive phase guard.
    std::uint64_t attributed_calls() const {
        return m_attributed_calls;
    }
    // Const runtime diagnostic: numeric state restores needing a registry lookup.
    std::uint64_t registry_lookups() const {
        return m_registry_lookups;
    }
    template<typename Function>
    void use(Function function, const Program& program) {
        bind(program);
        // Both operands are plain bool loads. A seen program takes one branch,
        // without a lookup, TLS access, guard construction or phase-stack access.
        if (static_cast<unsigned>(shader_first_use_collection_enabled) &
            static_cast<unsigned>(m_bound->pending)) {
            use_attributed(function, program.name);
            return;
        }
        function(program.name);
    }
    template<typename Function>
    void use(Function function, std::uint32_t program) {
        // Numeric names are needed for GL state restores. Owned programs retain
        // Program instead, so ordinary binds never enter this lookup path.
        if (program == 0) {
            use(function, Program{});
            return;
        }
        use(function, lookup(program));
    }
    template<typename Function, typename... Args>
    void submit(Function function, Args&&... args) {
        if (m_bound->pending) {
            // Disabled collection still consumes first submission. Later frames
            // must not attribute a program which has already reached the driver.
            m_bound->pending = false;
            if (shader_first_use_collection_enabled) {
                submit_attributed(function, std::forward<Args>(args)...);
                return;
            }
        }
        function(std::forward<Args>(args)...);
    }

private:
    LUMINUMBRA_FIRST_USE_COLD Program lookup(std::uint32_t program) {
        ++m_registry_lookups;
        const auto entry = m_programs.find(program);
        return {.name = program, .state = entry == m_programs.end() ? &no_program : &entry->second};
    }
    template<typename Function>
    LUMINUMBRA_FIRST_USE_COLD void use_attributed(Function function, std::uint32_t program) {
        ++m_attributed_calls;
        // Binding and submission have separate scopes so intervening nested
        // phases keep their normal lifetimes. Neither consumes the other.
        const PhaseScope phase(Phase::ShaderFirstUse);
        function(program);
    }
    template<typename Function, typename... Args>
    LUMINUMBRA_FIRST_USE_COLD void submit_attributed(Function function, Args&&... args) {
        ++m_attributed_calls;
        const PhaseScope phase(Phase::ShaderFirstUse);
        function(std::forward<Args>(args)...);
    }
    std::unordered_map<std::uint32_t, State> m_programs;
    State* m_bound = &no_program;
    std::uint64_t m_attributed_calls = 0;
    std::uint64_t m_registry_lookups = 0;
};
} // namespace Luminumbra::Client::Attribution

#undef LUMINUMBRA_FIRST_USE_COLD
