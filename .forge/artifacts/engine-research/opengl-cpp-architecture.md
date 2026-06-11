# Literature Survey: Modern OpenGL, C++ DOD, Job Systems, Engine Framework Architecture

Research artifact for the standing per-iteration optimization wave and the
framework-hardening iteration (long-range-roadmap.md §"Standing optimization
discipline"). Surveyed 2026-06-11. Every takeaway is tied to a concrete
Luminumbra system or roadmap item. Engine facts referenced below were
verified against the current tree: per-chunk `glDrawElements` in
`GBufferPass.cpp:170` / `ShadowPass.cpp:95`, per-region draws in
`FarLodSystem.cpp:411`, per-chunk VAO/VBO/EBO in `RenderPipeline.h`
(`ChunkRenderData`), mutex+condvar dual-`std::queue` JobSystem with
`std::function` jobs and `shared_ptr<atomic<int>>` handles
(`JobSystem.h`), EnTT views-only usage (no groups anywhere), marching-cubes
step-LOD with boundary skirts (`MarchingCubes.h`).

---

## Area 1 — Modern OpenGL performance (AZDO and after)

### Annotated sources

1. **"Approaching Zero Driver Overhead in OpenGL"** — Everitt, McDonald
   (NVIDIA), Sellers (AMD), Foley (Intel), GDC 2014.
   Slides: https://www.khronos.org/assets/uploads/developers/library/2014-gdc/Khronos-OpenGL-Efficiency-GDC-Mar14.pdf
   Video: https://gdcvault.com/play/1020791/ — the canonical playbook:
   persistent-mapped buffers, MultiDrawIndirect, bindless/array textures;
   claims up to 10x driver-overhead reduction. Cross-vendor authored, so its
   recommendations are the safe GL 4.x subset.
2. **"Beyond Porting"** — Cass Everitt, John McDonald (NVIDIA), Steam Dev
   Days 2014. http://media.steampowered.com/apps/steamdevdays/slides/beyondporting.pdf
   — the state-change cost hierarchy with actual throughput numbers (see
   takeaway 1). The single most useful slide deck for deciding what to batch.
3. **"High Performance Voxel Engine: Vertex Pooling"** — Nick McDonald
   (nickmcd), 2021. https://nickmcd.me/2021/04/04/high-performance-voxel-engine/
   — the AZDO recipe applied to exactly our problem: one persistent-mapped
   VBO divided into fixed-size buckets + one `glMultiDrawElementsIndirect`
   for all chunks. ~2x static render throughput vs per-chunk VAOs; 15-25%
   faster dynamic remeshing; over-provisioned buckets (3-7x) cost ~nothing.
4. **"GPU-Driven Rendering Pipelines"** — Ulrich Haar (Ubisoft), Sebastian
   Aaltonen (RedLynx), SIGGRAPH 2015 Advances in Real-Time Rendering.
   https://advances.realtimerendering.com/s2015/ — cluster-based GPU culling
   feeding indirect draws; 1-2 orders of magnitude fewer draw calls in
   AC: Unity; 30-80% of shadow triangles culled. Aaltonen later noted the
   DX11 (binding-model-equivalent) port was poor — calibrates how far to
   push GPU-driven on GL 4.x.
5. **"Buffer Object Streaming"** — Khronos OpenGL wiki.
   https://www.khronos.org/opengl/wiki/Buffer_Object_Streaming — the
   reference for fence-synced ring-buffer streaming over a persistent map.
6. **"Persistent Mapped Buffers in OpenGL" (+ benchmark follow-up)** —
   Bartlomiej Filipek, 2015. https://www.cppstories.com/2015/01/persistent-mapped-buffers-in-opengl/
   — practical triple-buffer + `glFenceSync`/`glClientWaitSync` pattern with
   benchmarks; documents the COHERENT vs explicit-flush trade.
7. **"Bindless Texturing for Deferred Rendering and Decals"** — Matt
   Pettineo (MJP), 2016. https://mynameismjp.wordpress.com/2016/03/25/bindless-texturing-for-deferred-rendering-and-decals/
   — what bindless actually buys and its debugging/validation costs
   (RenderDoc capture problems on AMD GL bindless are real).
8. **"The Truth on OpenGL Driver Quality"** — Rich Geldreich (ex-Valve),
   2014. http://richg42.blogspot.com/2014/05/the-truth-on-opengl-driver-quality.html
   — vendor-by-vendor GL driver reality (NVIDIA = fast but permissive and
   threaded; AMD = stricter conformance, historically slower GL path; Intel
   = most conservative). Old but the vendor characters have not changed.
9. **"A Trip Through the Graphics Pipeline 2011"** — Fabian Giesen.
   https://fgiesen.wordpress.com/2011/07/09/a-trip-through-the-graphics-pipeline-2011-index/
   — mental model for *why* state changes cost what they cost; required
   background for interpreting our GPU timer instrumentation.
10. **"GPU-Based Clay Simulation and Ray-Tracing Tech in Claybook"** —
    Sebastian Aaltonen, GDC 2018.
    https://media.gdcvault.com/gdc2018/presentations/Aaltonen_Sebastian_GPU_Based_Clay.pdf
    — production SDF raymarcher (mip-mapped volume SDF, coarse-then-fine
    sphere tracing, shipped at 60fps incl. Switch). Primary evidence base
    for the iteration-4 SHIELD-RT spike and iteration-6 productionization.

### Key takeaways

1. **Memorize the state-change hierarchy and audit passes against it**
   (Beyond Porting, slide "relative cost of state changes"): render-target
   switches ~60K/s, program switches ~300K/s, texture binds ~1.5M/s, UBO
   binds ~10M/s, uniform updates cheapest. Implication for our extracted
   pass architecture: pass-level FBO/program changes (6-ish per frame) are
   irrelevant; the per-chunk loop *inside* GBufferPass/ShadowPass is where
   the budget goes. Per-chunk uniform updates (model matrix) are fine;
   per-chunk texture binds would not be. Wire this hierarchy into how we
   read the GPU timer + draw-call counters during the optimization wave.
2. **Per-chunk VAO bind + glDrawElements is the documented worst case for
   our draw pattern** (AZDO, Vertex Pooling). `ChunkRenderData` holds a
   VAO/VBO/EBO per chunk and GBufferPass/ShadowPass iterate them
   (`GBufferPass.cpp:170`, `ShadowPass.cpp:95`). The literature's direct
   replacement: one persistent-mapped pooled VBO with fixed-size buckets,
   one shared index strategy, draw the whole visible set with a single
   `glMultiDrawElementsIndirect` whose command buffer is rebuilt (CPU-side)
   per frame. Nick McDonald measured ~2x static-draw throughput on a voxel
   workload of our shape. This is the highest-leverage "render submission"
   item for the standing optimization wave and also collapses ShadowPass
   cascade re-draws into 4 MDI calls.
3. **The same pooling fixes the upload path, not just the draw path.**
   `MeshUploadFrameStats` (terrain_payload_copies/bytes, deferred-upload
   accounting) shows we stage and copy per-chunk into per-chunk VBOs with
   `glBufferData`-style orphaning. A persistent-mapped ring/bucket pool +
   fences (Khronos streaming wiki, Filipek) turns upload into a memcpy into
   a mapped pointer — no map/unmap calls, no driver allocation, and the
   "deferred upload" budgeting logic keeps working (it just defers memcpys
   + DAIC writes instead of glBufferSubData calls).
4. **MDI command-buffer rebuild gives frustum culling for free and is the
   stepping stone to GPU-driven.** Writing the indirect command array each
   frame *is* the culling pass (skip a chunk = skip its DAIC entry). Later,
   a compute shader can write that buffer instead (chunk bounds in an SSBO,
   GL 4.3 compute — we already require compute for sdf_generation.compute).
   AC:Unity-style cluster culling is overkill for our chunk granularity;
   chunk/region-bound culling on GPU is the right scope (Aokana 2025
   reaches the same conclusion for voxel open worlds:
   https://arxiv.org/pdf/2505.02017).
5. **Prefer texture arrays over bindless for the material LUT path.**
   Bindless (`ARB_bindless_texture`) is NVIDIA-strong but not core, has
   RenderDoc/AMD capture problems (MJP, ktstephano
   https://ktstephano.github.io/rendering/opengl/bindless), and our
   materials.json palette count is small. A `GL_TEXTURE_2D_ARRAY` with a
   material-index vertex attribute removes per-material binds with zero
   portability risk. Mark bindless "candidate, evidence required" in the
   optimization-wave rotation, not default.
6. **FarLodSystem is the second MDI customer.** `FarLodSystem.cpp:411`
   issues one `glDrawElements` per region mesh; region meshes are static
   between rebuilds, so they are the *easy* pooling case (no per-frame
   bucket churn). When SHIELD-RT absorbs the far field in iteration 6,
   the pooled far-tile path becomes the LOD-blend fallback — keep it cheap.
7. **Windows driver realities** (Geldreich + Beyond Porting): NVIDIA's GL
   driver spins up its own threaded server — main-thread GL call cost is
   partly hidden, so CPU-side draw loops look cheaper on NVIDIA than they
   will on AMD/Intel. Perf-regression baselines captured on one vendor do
   not transfer; the PerfRegression gate should record vendor/driver in
   the baseline blob (it already re-blesses deliberately — add vendor to
   the bless metadata). Also: NVIDIA tolerates sloppy GL that AMD rejects;
   keep running the RenderHealth gate on at least one AMD or Mesa device
   before iteration 6's raymarch ships.
8. **Claybook validates the SHIELD-RT bet** (Aaltonen GDC 2018): a
   mip-mapped volume SDF raymarched in compute beat rasterization for
   their workload and shipped on Switch. Transferable specifics for the
   iteration-4 spike: store far-field SDF as a 3D texture with a proper
   mip chain (coarse mips double as the acceleration structure for
   sphere-trace step clamping); trace at half-res with a coarse pass and
   refine; derive normals from central differences of the same texture
   (no second data structure). The spike's evidence bar should be
   "raymarch cost at >F2 distances vs the measured F1/F2 tile draw+memory
   cost", which the GPU timers already measure on one side.

---

## Area 2 — C++ data-oriented design & memory

### Annotated sources

1. **"Data-Oriented Design and C++"** — Mike Acton, CppCon 2014.
   https://www.youtube.com/watch?v=rX0ItVEVjHc (slides:
   https://github.com/CppCon/CppCon2014 ) — the canon: design around data
   transforms and the multiple case; cache-line-aware layout yields 10x.
2. **"Data-Oriented Design (Or Why You Might Be Shooting Yourself in The
   Foot With OOP)"** — Noel Llopis, Game Developer Magazine 2009.
   https://gamesfromwithin.com/data-oriented-design — the practical
   precursor; shorter and more prescriptive than Acton's talk.
3. **"ECS back and forth" series, esp. Part 2 (groups) and Part 9 (sparse
   sets)** — Michele Caini (skypjack, EnTT author).
   https://skypjack.github.io/2019-03-07-ecs-baf-part-2/ ,
   https://skypjack.github.io/2020-08-02-ecs-baf-part-9/ — first-party
   cost model for the exact library we use: views are cheap and lazy;
   owning groups buy "perfect SoA" iteration at the price of
   insert/remove bookkeeping and exclusive component ownership; author's
   own advice is to avoid groups until profiling proves a hot iteration.
4. **"Memory Allocation Strategies" series** — Ginger Bill (Odin author),
   2019. https://www.gingerbill.org/article/2019/02/08/memory-allocation-strategies-002/
   — clearest written treatment of linear/arena, stack, pool, free-list
   allocators and when each is correct.
5. **"Memory system" series** — Stefan Reinalter, Molecular Musings, 2011.
   https://blog.molecular-matters.com/2011/07/15/memory-system-part-4/ —
   production engine allocator architecture (policy-based, tagged,
   per-system arenas) from a shipped middleware engine.
6. **"Virtual Memory Tricks"** — Niklas Gray, Our Machinery blog, 2017.
   https://ruby0x1.github.io/machinery_blog_archive/post/virtual-memory-tricks/index.html
   — reserve-huge/commit-on-demand patterns that remove reallocation and
   pointer instability; directly applicable to chunk/mesh pools on Win32.
7. **"C++ atomics, from basic to advanced"** — Fedor Pikus, CppCon 2017.
   https://www.youtube.com/watch?v=ZQFzMfHIxng — measured costs of atomics
   and contended cache lines; false sharing pathology and
   `hardware_destructive_interference_size`.
8. **"Pitfalls of Object Oriented Programming"** — Tony Albrecht, Sony
   2009 (slides widely mirrored; follow-up "Revisited" GDC 2017) — the
   before/after numbers for AoS→SoA scene-graph transforms that Acton's
   talk assumes.

### Key takeaways

1. **EnTT: stay on views; do not adopt groups preemptively.** We use
   ~4 simple views (WaterSystem camera/source lookup, point-light
   collection at `RenderPipeline.cpp:909`, static-mesh draw at
   `GBufferPass.cpp:198`) over small entity counts. skypjack's own
   guidance: groups only after profiling shows a hot iteration, because
   owning groups impose component-ownership exclusivity (a framework-level
   API constraint) and per-add/remove bookkeeping. Revisit only if
   iteration-5 ecology/stimulus channels create per-tick iteration over
   thousands of creature entities — that is the first plausible group
   candidate (`registry.group<PlannerState>(entt::get<StimulusSample>)`).
2. **The meshing hot path's allocation pattern is the textbook arena
   case.** `PolygoniseTerrain` populates per-chunk `mesh_vertices` /
   `mesh_indices` vectors; every remesh re-grows heap vectors on a worker
   thread, and skirt generation appends more. A per-worker arena (reset
   per job, gingerbill's linear allocator; reserve/commit via Gray's
   virtual-memory trick) eliminates allocator contention across JobSystem
   workers — malloc under multi-thread contention is exactly where
   general-purpose allocation "actually matters" (Acton, Reinalter). Tag
   this for the optimization wave's "memory (pooling/arenas)" rotation
   with a before/after on `TerrainMeshBuildStats.elapsed_us`.
3. **SoA decision rule for our loops** (Acton/Albrecht): convert to SoA
   only fields the inner loop touches. Concretely: streaming candidate
   collection (distance-sort over chunk coords) wants a flat
   `{coord, dist_sq}` array, not iteration over chunk objects; the
   marching-cubes cell loop reads SDF samples + writes vertices and is
   already effectively SoA via the density array — the win there is
   layout (Z-curve or slab ordering for the 8-corner fetch), not AoS→SoA.
   Don't SoA-convert cold structs (ChunkRenderData etc.) — that is
   cargo-culting the talk.
4. **False sharing audit before clever scheduling** (Pikus): our
   `JobHandle` counters are `shared_ptr<atomic<int>>` — heap-scattered
   atomics that worker threads decrement; multiple handles can land on
   one cache line via the allocator. A pooled, cache-line-aligned
   (`alignas(64)`) counter slab is cheap insurance and removes two heap
   allocations per batch. Stats counters (`TerrainMeshBuildStats` written
   from jobs) deserve the same audit.
5. **Per-job `std::function` is a hidden allocator + indirect-call tax**
   (Acton's "know your data" applied to the job payload): capturing
   lambdas over ~3 pointers fit SBO, but larger captures heap-allocate
   per dispatch, thousands of times per streaming burst. A fixed-size
   POD job struct (fn ptr + 48-64 bytes payload) in a pooled ring (see
   Area 3, Molecular Musings part 2: allocator change alone = 1.9x) is
   the prerequisite for any lock-free queue work.
6. **Adopt `tm`-style "meatier functions" at hot API boundaries** (Gray,
   Physical Design; Acton "where there's one, there's many"): e.g.,
   `SHIELD_WorldSystem` SDF sampling for the iteration-5 wind grid and
   iteration-6 Aetheric field sampling API should be batch queries
   (`sample_n(points[], out[])`), not per-point virtual/exported calls —
   this is both a perf rule and a framework-hardening API-surface rule.
7. **Measure with the data you ship** (Acton): the optimization wave's
   canonical currency is release-lane numbers (T-I3-20) — the literature
   unanimously backs this; debug-lane wins on cache behavior are noise.

---

## Area 3 — Job systems & parallelism

### Annotated sources

1. **"Parallelizing the Naughty Dog Engine Using Fibers"** — Christian
   Gyrling, GDC 2015.
   https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf
   — fiber job system, counter-based dependencies (jobs wait on atomic
   counters, not handles/futures), frame-centric memory (per-frame linear
   allocators tagged by lifetime).
2. **"Multithreading the Entire Destiny Engine"** — Barry Genova, GDC 2015.
   https://gdcvault.com/play/1022164/ — whole-frame job graph; runtime
   validation that every data access happens in a declared frame phase —
   the model for making lockstep determinism and parallelism coexist.
3. **"Job System 2.0: Lock-Free Work Stealing" series (parts 1-3)** —
   Stefan Reinalter, Molecular Musings, 2015.
   https://blog.molecular-matters.com/2015/09/25/job-system-2-0-lock-free-work-stealing-part-3-going-lock-free/
   — the measured ladder we should climb: mutex queue 18.5ms → pooled job
   allocator 9.9ms → Chase-Lev lock-free stealing 2.93ms (6.3x) on the
   same workload; exact memory-ordering pitfalls documented per operation.
4. **"Dynamic Circular Work-Stealing Deque"** — Chase & Lev, SPAA 2005 —
   the algorithm underneath every game work-stealing scheduler; cite for
   the correctness argument, implement from Reinalter/Le et al.
5. **"Bounded MPMC queue"** — Dmitry Vyukov, 1024cores.
   https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
   — the practical alternative when stealing is overkill: one array, two
   counters, per-cell sequence numbers; trivially supports our two-lane
   model as two queues.
6. **"Job System and ParallelFor"** — Krzysztof Narkowicz, 2017.
   https://knarkowicz.wordpress.com/2017/04/02/job-system-and-parallelfor/
   — pragmatic counterpoint: a simple global queue is fine until job
   counts are large and tiny; matches "measure before going lock-free".
7. **"FrameGraph: Extensible Rendering Architecture in Frostbite"** —
   Yuriy O'Donnell, GDC 2017. https://www.gdcvault.com/play/1024612/ —
   passes declare reads/writes; graph derives order, culls dead passes,
   aliases transient resources. Companion deep-dive: Hans-Kristian
   Arntzen, https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/.
8. **"FiberTaskingLib"** — RichieSams (open-source Gyrling-model
   implementation). https://github.com/RichieSams/FiberTaskingLib —
   reference code if we ever need wait-without-blocking-a-worker.

### Key takeaways

1. **Climb Reinalter's ladder in order; the first rung is allocation, not
   locks.** Our JobSystem is a single mutex + condvar over two
   `std::queue<std::function>` lanes. Reinalter's data says replacing
   per-job allocation with a pooled POD job ring buys ~1.9x *before*
   touching the lock; lock-free stealing buys the next ~3x only at high
   job counts. Optimization-wave sequencing: (a) POD job + pooled
   counters, (b) profile queue contention during streaming bursts
   (instrument time-in-`m_queue_mutex`), (c) only then Vyukov MPMC or
   Chase-Lev per-worker deques. Do not start at (c).
2. **Counters, not handle waits, for the chunk pipeline** (Gyrling): the
   gen → mesh → upload chain should be expressed as jobs decrementing a
   dependency counter that auto-enqueues the next stage, instead of any
   code calling `JobSystem::wait()` mid-pipeline. Our `JobHandle` already
   carries an atomic counter — extend it to "when zero, enqueue
   continuation job" and the dependency-aware chunk pipeline falls out
   without fibers. Fibers themselves (stack switching) are *not*
   justified at our job granularity; Gyrling needed them for 800+
   fine-grained gameplay jobs per frame.
3. **Keep two lanes, but recognize them as a policy, not a mechanism.**
   The High/Normal starvation guard (`kNormalServiceInterval = 4`) is a
   reasonable work-sharing policy; the literature (Narkowicz) supports
   simple priority lanes at our scale. When moving to per-worker deques,
   preserve lane semantics by stealing High-lane first — don't let a
   lock-free refactor silently delete the starvation guarantee that
   streaming latency (player-view coverage mandate) depends on.
4. **Destiny's phase-validation idea maps onto our determinism gates:**
   declare which tick phase may write chunk SDF vs read it for meshing,
   and assert it in debug (their "rigorous validation of multithreaded
   data lifecycles"). This is cheap (a phase enum + thread-local current
   phase) and converts data races into deterministic gate failures —
   directly strengthens the world-hash desync oracle planned for
   iteration 4 Wave C and the 30 Hz SimulationClock contract.
5. **Memory-ordering discipline if/when we go lock-free** (Reinalter
   part 3): Pop needs a full fence (store-load reordering), Push needs
   only release, Steal's CAS is the barrier. Use C++11 acquire/release
   atomics, not volatiles or platform intrinsics; wraparound is a
   non-issue with 64-bit indices.
6. **A frame-graph-lite is the right formalization of our extracted pass
   architecture — but as declared inputs/outputs, not a scheduler.**
   O'Donnell's wins were transient-resource aliasing and automatic
   barriers; on GL 4.x the driver owns both, so a full frame graph buys
   little at 8-ish passes (Arntzen reaches the same conclusion). What IS
   worth stealing for the framework-hardening iteration: each pass
   declares the textures/FBOs it reads/writes in a struct, the pipeline
   validates the chain (RenderHealth gate can diff declared vs actual GL
   bindings), and pass insertion (iteration-5 weather volumes,
   iteration-6 raymarch pass) becomes a registration instead of editing
   `RenderPipeline::render` by hand.
7. **Watch JobSystem stats for the lane-balance rotation:** we already
   export per-lane queue depths in `RuntimeStats`; add
   jobs-executed-per-worker and steal/contention counters when refactoring
   so the optimization wave's "JobSystem scheduling/lane balance" focus
   has evidence, per the gate-first discipline.

---

## Area 4 — Engine framework architecture (framework-hardening iteration)

### Annotated sources

1. **"Physical Design of The Machinery"** — Niklas Gray, 2017.
   https://ruby0x1.github.io/machinery_blog_archive/post/physical-design/index.html
   — the radical include rule (headers include nothing), opaque handle
   structs, C API boundary; the most concrete published header-hygiene
   regime from a shipped engine team (ex-Bitsquid/Stingray).
2. **"API Versioning"** — Niklas Gray, 2021.
   https://ruby0x1.github.io/machinery_blog_archive/post/api-versioning/index.html
   — semver on API structs; additive = minor, layout/signature = major;
   plugins request `get("api", {1,0,0})` and fail gracefully. The
   template for our "versioned contract/gate patterns" roadmap line.
3. **"The Story behind The Truth: Designing a Data Model"** — Niklas Gray,
   2018. https://ourmachinery.com/post/the-story-behind-the-truth-designing-a-data-model/
   — centralized object/property data model giving serialization, undo,
   prototypes, collaboration "for free"; the strongest published argument
   for data-first (vs API-first-around-objects) engine boundaries.
4. **Bevy plugin architecture** — https://bevy.org/learn/quick-start/getting-started/plugins/
   (+ https://taintedcoders.com/bevy/plugins ) — everything, including
   engine internals, is a plugin over a minimal app core; demonstrates
   lifecycle discipline (build/ready/finish/cleanup) and the failure mode
   (no first-class inter-plugin dependency declaration → ordering bugs).
5. **Godot engine architecture docs** ("custom modules in C++",
   core/servers/scene layering) — https://docs.godotengine.org/en/stable/contributing/development/core_and_modules/
   — the opposite pole: monolithic core with a hard server/scene split;
   instructive for which boundary (engine "servers" vs game "scene") to
   copy for the engine/game split-lint.
6. **"CMake 3.16 added support for precompiled headers & unity builds"** —
   Viktor Kirilov (doctest author), 2019.
   https://onqtam.github.io/programming/2019-12-20-pch-unity-cmake-3-16/
   — measured guidance for the exact build system we use: PCH 20-30%
   (gcc/clang) to 50%+ (MSVC); `UNITY_BUILD` examples at 3-4x; lists ODR
   hazards unity builds introduce.
7. **"UE4 #includes, Precompiled Headers and IWYU"** — kantandev.
   http://kantandev.com/articles/ue4-includes-precompiled-headers-and-iwyu
   — how the largest C++ engine codebase migrated to
   include-what-you-use discipline while keeping shared PCHs; the
   coexistence strategy (IWYU for correctness, PCH for speed) is the one
   to copy.
8. **include-what-you-use** — https://include-what-you-use.org/ — the
   tool itself; clang-based, CMake-integrable, supports mapping files for
   third-party umbrella headers (glad, glm, EnTT need mappings).
9. **"It's All About The Data"** — Niklas Gray, 2017.
   https://ruby0x1.github.io/machinery_blog_archive/post/its-all-about-the-data/index.html
   — subsystem lifecycle and API minimalism rationale that complements
   the physical-design post.

### Key takeaways

1. **The framework-hardening exit criterion ("new game module builds
   against public headers + data only") is exactly The Machinery's
   plugin contract** — adopt their two enforcement mechanisms, not their
   C-only style: (a) public headers live in `include/luminumbra/` and may
   include only other public headers (we already have
   `include/luminumbra/core/Types.h` — extend the convention and lint
   it); (b) anything the game reaches for that isn't public is a build
   error, enforced by target-level CMake include dirs (`PRIVATE` vs
   `PUBLIC` on `src/` paths), making common/client/server layering
   "enforced by build, not convention" per the roadmap line.
2. **Version the gate/validator contract like Gray versions APIs:**
   validator modes as `{major.minor}` where adding a check = minor,
   changing a threshold/semantic = major requiring deliberate re-bless —
   this formalizes the existing "determinism contracts only move via
   deliberate versioned bumps" spine rule into one mechanical scheme, and
   the same struct-versioning rules cover save-format and replay-stream
   compatibility for the committed session-replay expansion.
3. **`RenderPipeline.h` is the include-hygiene worst offender and the
   cheapest demo win:** it includes glad, glm, Chunk.h, AssetManager.h,
   LightingComponents.h into every consumer while *also* forward-declaring
   half its types. Applying Gray's rule (opaque `ChunkRenderData` handles,
   GL types as `u32` — already half-done — and moving GBuffer/ShadowMap
   structs into the .cpp or a private header) decouples renderer
   recompiles from world recompiles. Run IWYU with mappings for
   glad/glm/EnTT across `src/` as a framework-hardening task; gate on
   "public headers pass IWYU clean".
4. **Subsystem lifecycle unification has a published shape:** Bevy's
   build/ready/finish/cleanup and The Machinery's load/init/tick/shutdown
   converge on: registration is separate from initialization; tick order
   is explicit data (a registered list with declared dependencies), not
   call-site order in `main_client.cpp` / `GameSession.cpp`. Our
   TickSimulation "field-budget slot" plan (iteration 6) already implies
   this — formalize an `ISimSystem {init, tick(dt), shutdown}` registry
   in the hardening pass so wind grid / weather / fields slot in
   uniformly instead of each adding a call site.
5. **Don't adopt The Truth wholesale; adopt its boundary lesson.** A
   centralized object/property store pays off for *editor*/collaboration
   workloads The Machinery targeted. Our equivalent decision: the
   engine/game data boundary stays JSON-schema'd data files (materials,
   archetypes, biome palettes) + versioned snapshot structs — i.e., keep
   "engine knows only emissive scalar fields" (memory: engine/game
   decoupling) and resist inventing a runtime reflection store the game
   doesn't need.
6. **Build-time strategy for our CMake tree:** enable
   `target_precompile_headers` per module (glm + std + EnTT for common;
   + glad for client) and trial `CMAKE_UNITY_BUILD` on
   luminumbra_common first (Kirilov: biggest wins on many-small-TU
   targets, which `sources.cmake` listings suggest we are). Two cautions
   from the literature: unity builds mask missing includes (run IWYU in
   CI on non-unity config) and inflate incremental single-file rebuilds —
   measure both full and incremental, since dev-loop latency is what the
   optimization wave actually feels.
7. **Module boundary precedent for the split-lint:** Godot's
   servers-never-include-scene rule is our common-never-includes-client/
   server rule; encode it as a CMake-level link/include check (a lint that
   greps include paths per target is adequate) so the engine/game
   split-lint covers *layering* as well as content references.

---

## Decisions this research should change

1. **Stop planning around per-chunk GL buffers.** The current
   `ChunkRenderData` one-VAO-per-chunk design is the pattern AZDO/vertex
   pooling exists to delete. The standing optimization wave's "render
   submission" rotation should target pooled persistent-mapped buckets +
   `glMultiDrawElementsIndirect` as one coherent work item (draw + upload
   + shadow paths together), not incremental batching tweaks to the
   existing per-chunk loop — the literature says the architecture change
   is where the 2x lives, and intermediate half-steps get discarded.
2. **Demote bindless textures from "candidate" to "only with vendor
   evidence".** GL bindless is NVIDIA-comfortable, AMD-fragile
   (tooling/capture), Intel-absent; texture arrays cover our material
   cardinality. The roadmap's "bindless candidates" line should read
   "texture-array consolidation first; bindless only if array limits are
   actually hit on measured hardware."
3. **Re-order the JobSystem optimization sequence.** If any plan assumed
   "go lock-free" as the JobSystem improvement, the measured ladder
   (Molecular Musings) says: pooled POD jobs + pooled aligned counters
   first (~2x, low risk, helps determinism by removing allocator
   nondeterminism in timing), contention measurement second, lock-free
   structures only with evidence. `std::function`/`shared_ptr` in the
   dispatch path is the actual current bottleneck candidate, not the
   mutex.
4. **Add vendor/driver identity to PerfRegression baseline metadata.**
   NVIDIA's threaded GL driver makes CPU-submission costs
   vendor-dependent; baselines and ratchets are otherwise comparing
   different machines' driver architectures, undermining the re-bless
   protocol's meaning.
5. **Don't build a full frame graph; build pass I/O declarations.** The
   Frostbite model's headline wins (transient aliasing, barriers) don't
   exist on GL 4.x. The framework-hardening iteration should scope
   "task-graph architecture" down to declared pass inputs/outputs +
   validation + data-driven pass order — cheaper, and it still unlocks
   clean insertion of the iteration-5 weather and iteration-6 raymarch
   passes.
6. **Fibers are explicitly rejected for our scale.** Counter-chained
   continuations (Gyrling's dependency counters without his fibers) give
   the dependency-aware gen→mesh→upload pipeline; stack-switching
   machinery is unjustified at hundreds-of-jobs-per-frame granularity
   and would complicate the 30 Hz determinism story.
7. **The framework-hardening iteration gains two concrete, literature-
   backed exit artifacts** beyond the existing criterion: (a) IWYU-clean
   public header set with header-includes-only-public-headers lint;
   (b) a semver'd API/gate contract document (Gray's additive-minor /
   breaking-major rules) covering validator modes, snapshot structs, and
   the replay stream — turning "versioned contract" from a phrase into a
   mechanical rule set.
8. **EnTT groups are deferred by the library author's own advice** — any
   plan line reading "ECS iteration layout" should mean measuring view
   iteration in release lane and considering groups only for the
   iteration-5 creature/stimulus hot loop, because owning groups impose
   API-visible ownership constraints that would otherwise leak into the
   framework-hardening public API design.
