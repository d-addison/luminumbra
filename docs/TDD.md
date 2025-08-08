# Test-Driven Development (TDD) Guide for Luminumbra – The Quantum Engine

**Version:** 1.0  
**Status:** Draft (Pre-Production)  
**Author:** Solo Developer  
**Last Updated:** July 31, 2025

> "Every system must be testable at every level — from the voxel to the vision."

This document defines the Test-Driven Development framework for Luminumbra. It is designed for a solo developer who values precision, performance, and clarity. The goal is not just to write tests, but to build confidence in every change through verifiable behavior.

---

## 1. Core TDD Philosophy: "Testability by Design"

Luminumbra’s architecture supports testing at its core. This document ensures that testability remains central throughout development.

### Principles

- **Determinism is Non-Negotiable**: All simulation logic must produce identical results given the same inputs.
- **Isolation First**: Each unit test must be independent and self-contained.
- **Traceability Over Coverage**: A failing test should reveal why it failed — not just that it did.
- **Fast Feedback Loop**: Tests must run quickly enough to integrate into real-time development.

> If you cannot write a failing test before writing code, the behavior is not defined.

---

## 2. The TDD Cycle

```text
1. Define behavior (spec) →
2. Write failing test →
3. Implement minimal fix →
4. Run test → pass? ✔️ → refactor → done
```

> ✅ Every new feature must begin with a test that fails.

---

## 3. Testing by System

### 3.1. SHIELD Engine: World Generation & Streaming

**Key Test Types**

- **SDF Sampling**: Validate mathematical correctness of procedural generation.
- **Chunk Consistency**: Ensure identical output from same seed across runs.
- **Memory Safety**: Use AddressSanitizer to detect leaks during chunk streaming.

**Pattern**
Use JSON files in `test/data/shield/` to store golden reference data (e.g., expected voxel values).

---

### 3.2. Instinct Engine: AI Behavior

**Key Test Types**

- **Action Validity**: Does `MoveToTarget` fail gracefully without a target?
- **Plan Generation**: Given hunger and thirst, does the agent generate a valid sequence of actions?
- **Determinism**: Same inputs → identical plan output.

**Tool**
Write Lua scripts in `scripts/test/ai/` to simulate scenarios. Run via `busted`.

---

### 3.3. Atmospheric Engine: Weather & Seasons

**Key Test Types**

- **Wind Field**: Verify decay and directionality.
- **Weather Triggering**: Does rain start at the correct time?
- **Determinism**: Same seed → identical weather state after 100 ticks.

---

### 3.4. Aetheric Field: Light & Energy Flow

**Key Test Types**

- **Emission Decay**: Verify energy levels drop over time.
- **Propagation Accuracy**: Does a crystal’s light affect nearby entities?
- **Visual Validation**: Render debug field and compare to reference image.

---

## 4. ECS Core Testing (EnTT)

- **Entity Lifecycle**: Create, destroy, query — ensure no memory issues.
- **Component Storage**: Verify tight packing and cache coherency.
- **System Order**: Validate that systems run in the correct dependency order.
- **Event Bus**: Ensure events are delivered exactly once.

---

## 5. Integration & Regression Testing

### The "Living Diorama" Test

> _A player spawns at dawn. A grovestrider appears, senses hunger, and walks to a berry bush._

Steps:

1. Initialize world with known seed.
2. Spawn player entity.
3. Run AI step.
4. Validate:
   - Agent has `NeedsComponent` with high hunger.
   - Plan includes `Eat`, then `Drink`.
   - Path is collision-free.

Failure triggers full debug log: component states, inputs, time steps.

---

## 6. Lua Scripting & Hot-Reload Testing

**Best Practices**

- Write tests in `scripts/test/`.
- Use `sol2` to call Lua functions from C++.
- Mock `world`, `eventbus`, and `registry`.

Example:

```lua
local ai = require("ai.plan")

function test_plan_generation()
    local agent = {needs = {hunger = 0.8, thirst = 0.6}}
    local plan = ai.generate_plan(agent)
    assert(plan[1].action == "Eat")
end
```

Run via:

```bash
busted scripts/test/ai/plan_test.lua
```

---

## 7. Tooling

**CMake Setup**
Add to `CMakeLists.txt`:

```cmake
enable_testing()
add_subdirectory(test)

add_executable(ai_test tests/ai/test_ai.cpp)
target_link_libraries(ai_test luminumbra_common EnTT Catch2::Catch2)
add_test(NAME ai_test COMMAND ai_test)
```

**CI Pipeline**
Run nightly:

- `make test`
- Benchmark frame rate and memory usage.

---

## 8. Developer Workflow

- Run `make test` before committing.
- Write a failing test for every new feature.
- Use `assert_debug()` to log state during failure — no `std::cout`.
- Tag tests: `[slow]`, `[visual]`.

> If you can't reproduce the bug in a test, you don’t own it.

---

## 9. Appendix

### Sample Test Files

`test/shield/test_chunk_generation.cpp`

```cpp
#include <gtest/gtest.h>
#include "luminumbra/world/Chunk.h"
#include "luminumbra/world/MarchingCubes.h"

TEST(ChunkGenerationTest, GeneratesValidMesh) {
    Chunk chunk{10, 10, 10, 0};
    auto mesh = marching_cubes(chunk);
    EXPECT_GT(mesh.vertices.size(), 0);
    EXPECT_EQ(mesh.indices.size() % 3, 0);
}
```

`scripts/test/ai/test_plan_generation.lua`

```lua
local ai = require("ai.plan")

describe("Plan Generation", function()
    it("should generate Eat then Drink", function()
        local agent = {needs = {hunger = 0.8, thirst = 0.6}}
        local plan = ai.generate_plan(agent)
        assert(plan[1].action == "Eat")
        assert(plan[2].action == "Drink")
    end)
end)
```

---

## Final Note

In Luminumbra, every system must be verifiable. TDD is not a phase — it’s the foundation of reliability.

> “If you can’t test it, you don’t own it.”
