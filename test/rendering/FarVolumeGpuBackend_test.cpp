#include "luminumbra_client/rendering/FarVolumeGpuBackend.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;

enum class Call {
    SnapshotVao,
    SnapshotVbo,
    CreateVao,
    CreateBuffer,
    BindVao,
    BindVertex,
    BindIndex,
    UploadVertex,
    UploadIndex,
    QueryVertex,
    QueryIndex,
    Attribute,
    Draw,
    Enable
};

// Fixed storage keeps every driver operation noexcept without allocations.
// EBO binding belongs to each VAO; attribute pointers capture ARRAY_BUFFER.
struct FakeDriver final : FarVolumeGpuDriver {
    struct Attribute {
        std::uint32_t vbo = 0;
        std::int32_t stride = 0;
        std::size_t offset = 0;
        int components = 0;
        bool integer = false, enabled = false;
    };
    struct Vao {
        bool live = false;
        std::uint32_t ebo = 0;
        std::array<Attribute, 3> attributes{};
    };
    struct Buffer {
        bool live = false;
        std::int64_t bytes = 0;
        std::array<unsigned char, 1024> data{};
    };
    std::array<Vao, 64> vaos{};
    std::array<Buffer, 64> buffers{};
    std::array<unsigned, 64> vao_deletes{}, buffer_deletes{};
    std::array<bool, 64> acquired_vaos{}, acquired_buffers{};
    std::array<Call, 64> calls{};
    std::uint32_t vao = 1, vbo = 2, next_name = 8;
    const std::thread::id owner = std::this_thread::get_id();
    const int context = 1;
    int current_context = 1;
    bool loaded = true, error = false, bad_call = false, delete_error = false;
    bool fail_draw = false;
    mutable unsigned validity_calls = 0;
    unsigned operation_count = 0, error_reads = 0;
    unsigned fail_at = 0, zero_at = 0, wrong_size_at = 0;
    std::int64_t wrong_size = -1;

    FakeDriver() {
        vaos[1].live = true;
        vaos[1].ebo = 3;
        vaos[1].attributes[0] = {4, 28, 0, 3, false, true};
        buffers[2].live = buffers[3].live = buffers[4].live = true;
        buffers[2].bytes = 17;
        buffers[3].bytes = 19;
    }
    ~FakeDriver() override {
        ExpectBalanced();
    }
    void ExpectBalanced() const {
        EXPECT_FALSE(bad_call);
        for (std::size_t i = 8; i < vaos.size(); ++i) {
            EXPECT_FALSE(vaos[i].live) << i;
            EXPECT_FALSE(buffers[i].live) << i;
            EXPECT_EQ(vao_deletes[i], acquired_vaos[i] ? 1u : 0u) << i;
            EXPECT_EQ(buffer_deletes[i], acquired_buffers[i] ? 1u : 0u) << i;
        }
        EXPECT_EQ(vao_deletes[1], 0u);
        EXPECT_EQ(buffer_deletes[2], 0u);
        EXPECT_EQ(buffer_deletes[3], 0u);
        EXPECT_EQ(buffer_deletes[4], 0u);
    }
    void ExpectOldBindings() const {
        EXPECT_EQ(vao, 1u);
        EXPECT_EQ(vbo, 2u);
        EXPECT_TRUE(vaos[1].live);
        EXPECT_EQ(vaos[1].ebo, 3u);
        EXPECT_EQ(vaos[1].attributes[0].vbo, 4u);
        EXPECT_EQ(buffers[2].bytes, 17);
        EXPECT_EQ(buffers[3].bytes, 19);
    }
    void Step(Call call) noexcept {
        if (operation_count >= calls.size()) {
            bad_call = true;
            return;
        }
        calls[operation_count++] = call;
        if (operation_count == fail_at)
            error = true;
    }
    bool ValidContext() const noexcept override {
        ++validity_calls;
        return loaded && current_context == context && owner == std::this_thread::get_id();
    }
    bool HasError() noexcept override {
        ++error_reads;
        const bool previous = error;
        error = false;
        return previous;
    }
    void GetVertexArrayBinding(std::uint32_t& name) noexcept override {
        Step(Call::SnapshotVao);
        name = vao;
    }
    void GetVertexBufferBinding(std::uint32_t& name) noexcept override {
        Step(Call::SnapshotVbo);
        name = vbo;
    }
    void CreateVertexArray(std::uint32_t& name) noexcept override {
        Step(Call::CreateVao);
        if (operation_count == zero_at) {
            name = 0;
            return;
        }
        name = next_name++;
        vaos[name].live = acquired_vaos[name] = true;
    }
    void CreateBuffer(std::uint32_t& name) noexcept override {
        Step(Call::CreateBuffer);
        if (operation_count == zero_at) {
            name = 0;
            return;
        }
        name = next_name++;
        buffers[name].live = acquired_buffers[name] = true;
    }
    void DeleteVertexArray(std::uint32_t name) noexcept override {
        if (!name || !vaos[name].live)
            bad_call = true;
        ++vao_deletes[name];
        vaos[name].live = false;
        if (vao == name)
            vao = 0;
        if (delete_error)
            error = true;
    }
    void DeleteBuffer(std::uint32_t name) noexcept override {
        if (!name || !buffers[name].live)
            bad_call = true;
        ++buffer_deletes[name];
        buffers[name].live = false;
        if (vbo == name)
            vbo = 0;
        for (auto& state : vaos) {
            if (state.ebo == name)
                state.ebo = 0;
            for (auto& attribute : state.attributes)
                if (attribute.vbo == name)
                    attribute.vbo = 0;
        }
        if (delete_error)
            error = true;
    }
    void BindVertexArray(std::uint32_t name) noexcept override {
        Step(Call::BindVao);
        if (name && !vaos[name].live)
            bad_call = true;
        vao = name;
    }
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override {
        Step(kind == FarVolumeBufferKind::Vertex ? Call::BindVertex : Call::BindIndex);
        if (name && !buffers[name].live)
            bad_call = true;
        if (kind == FarVolumeBufferKind::Vertex)
            vbo = name;
        else {
            if (!vao)
                bad_call = true;
            vaos[vao].ebo = name;
        }
    }
    std::uint32_t Bound(FarVolumeBufferKind kind) const noexcept {
        return kind == FarVolumeBufferKind::Vertex ? vbo : vaos[vao].ebo;
    }
    void UploadBuffer(FarVolumeBufferKind kind,
                      std::intptr_t bytes,
                      const void* data) noexcept override {
        Step(kind == FarVolumeBufferKind::Vertex ? Call::UploadVertex : Call::UploadIndex);
        auto& buffer = buffers[Bound(kind)];
        if (!buffer.live || bytes < 0 || std::size_t(bytes) > buffer.data.size()) {
            bad_call = true;
            return;
        }
        buffer.bytes = bytes;
        std::memcpy(buffer.data.data(), data, std::size_t(bytes));
    }
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override {
        Step(kind == FarVolumeBufferKind::Vertex ? Call::QueryVertex : Call::QueryIndex);
        const auto& buffer = buffers[Bound(kind)];
        if (!buffer.live)
            bad_call = true;
        bytes = operation_count == wrong_size_at ? wrong_size : buffer.bytes;
    }
    void SetAttribute(FarVolumeVertexAttribute attribute,
                      std::int32_t stride,
                      std::size_t offset) noexcept override {
        Step(Call::Attribute);
        if (!vao || !vbo || !buffers[vbo].live)
            bad_call = true;
        const bool integer = attribute == FarVolumeVertexAttribute::Material;
        vaos[vao].attributes[std::size_t(attribute)] = {
            vbo, stride, offset, integer ? 1 : 3, integer, false};
    }
    bool DrawIndexed(std::int32_t count) noexcept override {
        Step(Call::Draw);
        if (count <= 0 || !vao || !vaos[vao].live)
            bad_call = true;
        return !fail_draw;
    }
    void EnableAttribute(FarVolumeVertexAttribute attribute) noexcept override {
        Step(Call::Enable);
        if (!vao)
            bad_call = true;
        vaos[vao].attributes[std::size_t(attribute)].enabled = true;
    }
};

FarVolumeRenderMesh Mesh() {
    FarVolumeRenderMesh mesh;
    mesh.content = World::FarVolumeContent::Mixed;
    mesh.vertices = {{{0, 0, 0}, {0, 1, 0}, 7},
                     {{1, 0, 0}, {0, 1, 0}, 11},
                     {{1, 0, 1}, {0, 1, 0}, 13},
                     {{0, 0, 1}, {0, 1, 0}, 17}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    // Spare CPU capacity must not be counted or uploaded.
    mesh.vertices.reserve(32);
    mesh.indices.reserve(32);
    return mesh;
}

void ExpectSameInput(const FarVolumeRenderMesh& mesh, const FarVolumeRenderMesh& before) {
    EXPECT_EQ(mesh.content, before.content);
    EXPECT_EQ(mesh.indices, before.indices);
    ASSERT_EQ(mesh.vertices.size(), before.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        EXPECT_EQ(mesh.vertices[i].position, before.vertices[i].position);
        EXPECT_EQ(mesh.vertices[i].normal, before.vertices[i].normal);
        EXPECT_EQ(mesh.vertices[i].material_id, before.vertices[i].material_id);
    }
}

static_assert(!std::is_copy_constructible_v<FarVolumeGlAllocation>);
static_assert(!std::is_move_constructible_v<FarVolumeGlAllocation>);
static_assert(!std::is_copy_assignable_v<FarVolumeGlAllocation>);
static_assert(!std::is_move_assignable_v<FarVolumeGlAllocation>);
static_assert(std::is_same_v<decltype(UploadFarVolumeGpu(std::declval<const FarVolumeRenderMesh&>(),
                                                         std::declval<FarVolumeGpuDriver&>())),
                             std::unique_ptr<FarVolumeGpuAllocation>>);
} // namespace

TEST(FarVolumeGpuBackend, ExactStoresLayoutIndicesAndUniqueOwnership) {
    FakeDriver driver;
    const auto mesh = Mesh();
    const auto before = mesh;
    auto resource = UploadFarVolumeGpu(mesh, driver);
    ASSERT_TRUE(resource);
    const auto* allocation = dynamic_cast<const FarVolumeGlAllocation*>(resource.get());
    ASSERT_NE(allocation, nullptr);
    EXPECT_EQ(resource->bytes(), 4 * sizeof(VoxelVertex) + 6 * sizeof(std::uint32_t));
    EXPECT_EQ(allocation->index_count(), 6);
    const auto vao = allocation->native_vao();
    const auto& state = driver.vaos[vao];
    const auto vbo = state.attributes[0].vbo;
    const auto ebo = state.ebo;
    ASSERT_NE(vao, 0u);
    ASSERT_NE(vbo, 0u);
    ASSERT_NE(ebo, 0u);
    EXPECT_NE(vbo, ebo);
    EXPECT_EQ(driver.buffers[vbo].bytes, 4 * sizeof(VoxelVertex));
    EXPECT_EQ(driver.buffers[ebo].bytes, 6 * sizeof(std::uint32_t));
    EXPECT_EQ(std::memcmp(driver.buffers[vbo].data.data(),
                          mesh.vertices.data(),
                          mesh.vertices.size() * sizeof(VoxelVertex)),
              0);
    EXPECT_EQ(std::memcmp(driver.buffers[ebo].data.data(),
                          mesh.indices.data(),
                          mesh.indices.size() * sizeof(std::uint32_t)),
              0);
    const std::array<std::size_t, 3> offsets = {offsetof(VoxelVertex, position),
                                                offsetof(VoxelVertex, normal),
                                                offsetof(VoxelVertex, material_id)};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& attribute = state.attributes[i];
        EXPECT_EQ(attribute.vbo, vbo);
        EXPECT_EQ(attribute.stride, sizeof(VoxelVertex));
        EXPECT_EQ(attribute.offset, offsets[i]);
        EXPECT_EQ(attribute.components, i == 2 ? 1 : 3);
        EXPECT_EQ(attribute.integer, i == 2);
        EXPECT_TRUE(attribute.enabled);
    }
    driver.ExpectOldBindings();
    ExpectSameInput(mesh, before);
    auto moved = std::move(resource);
    EXPECT_FALSE(resource);
    EXPECT_EQ(moved.get(), allocation);
    moved.reset(); // Objects die before the fixture driver.
    driver.ExpectOldBindings();
    driver.ExpectBalanced();
}

TEST(FarVolumeGpuBackend, EveryOperationErrorRestoresStateAndDeletesAllAdoptedNames) {
    unsigned stages = 0;
    {
        FakeDriver baseline;
        auto resource = UploadFarVolumeGpu(Mesh(), baseline);
        ASSERT_TRUE(resource);
        stages = baseline.operation_count;
    }
    // Includes both snapshots, nonzero names returned with errors, every bind,
    // upload, size query, attribute operation, and both explicit restorations.
    for (unsigned stage = 1; stage <= stages; ++stage) {
        SCOPED_TRACE(stage);
        FakeDriver driver;
        driver.fail_at = stage;
        const auto mesh = Mesh();
        const auto before = mesh;
        EXPECT_FALSE(UploadFarVolumeGpu(mesh, driver));
        driver.ExpectOldBindings();
        driver.ExpectBalanced();
        ExpectSameInput(mesh, before);
        EXPECT_LE(driver.error_reads, stages + 1);
    }
}

TEST(FarVolumeGpuBackend, ZeroNamesAndWrongStorageSizesRefuseWithoutLeaks) {
    std::array<Call, 64> calls;
    unsigned stages = 0;
    {
        FakeDriver baseline;
        auto resource = UploadFarVolumeGpu(Mesh(), baseline);
        ASSERT_TRUE(resource);
        calls = baseline.calls;
        stages = baseline.operation_count;
    }
    for (unsigned i = 0; i < stages; ++i) {
        const auto call = calls[i];
        if (call == Call::CreateVao || call == Call::CreateBuffer) {
            SCOPED_TRACE(i + 1);
            FakeDriver driver;
            driver.zero_at = i + 1;
            EXPECT_FALSE(UploadFarVolumeGpu(Mesh(), driver));
            driver.ExpectOldBindings();
            driver.ExpectBalanced();
        }
        if (call == Call::QueryVertex || call == Call::QueryIndex) {
            for (const auto wrong : {-1, 0, 1, 1024}) {
                SCOPED_TRACE(i + 1);
                SCOPED_TRACE(wrong);
                FakeDriver driver;
                driver.wrong_size_at = i + 1;
                driver.wrong_size = wrong;
                EXPECT_FALSE(UploadFarVolumeGpu(Mesh(), driver));
                driver.ExpectOldBindings();
                driver.ExpectBalanced();
            }
        }
    }
}

TEST(FarVolumeGpuBackend, BoundPreviousAllocationSurvivesFailedReplacement) {
    // Resource/charge pairing is covered by FarVolumeUpload_test.cpp. Here the
    // previous resource is actually bound while a later backend upload fails.
    FakeDriver driver;
    auto previous = UploadFarVolumeGpu(Mesh(), driver);
    ASSERT_TRUE(previous);
    const auto* allocation = static_cast<const FarVolumeGlAllocation*>(previous.get());
    const auto old_vao = allocation->native_vao();
    const auto old_vbo = driver.vaos[old_vao].attributes[0].vbo;
    const auto old_ebo = driver.vaos[old_vao].ebo;
    const auto old_bytes = previous->bytes();
    driver.vao = old_vao;
    driver.vbo = old_vbo;
    // Fail after creating and uploading both new stores.
    driver.wrong_size_at = driver.operation_count + 12;
    EXPECT_FALSE(UploadFarVolumeGpu(Mesh(), driver));
    EXPECT_EQ(driver.vao, old_vao);
    EXPECT_EQ(driver.vbo, old_vbo);
    EXPECT_EQ(driver.vaos[old_vao].ebo, old_ebo);
    EXPECT_TRUE(driver.vaos[old_vao].live);
    EXPECT_TRUE(driver.buffers[old_vbo].live);
    EXPECT_TRUE(driver.buffers[old_ebo].live);
    EXPECT_EQ(driver.vao_deletes[old_vao], 0u);
    EXPECT_EQ(driver.buffer_deletes[old_vbo], 0u);
    EXPECT_EQ(driver.buffer_deletes[old_ebo], 0u);
    EXPECT_EQ(previous->bytes(), old_bytes);
    // The external renderer must unbind resources before retirement.
    driver.vao = 1;
    driver.vbo = 2;
    previous.reset();
    driver.ExpectOldBindings();
    driver.ExpectBalanced();
}

TEST(FarVolumeGpuBackend, ZeroOriginalBindingsAreRestoredWithoutElementBindOnDefaultVao) {
    FakeDriver driver;
    driver.vao = 0;
    driver.vbo = 0;
    auto resource = UploadFarVolumeGpu(Mesh(), driver);
    ASSERT_TRUE(resource);
    EXPECT_EQ(driver.vao, 0u);
    EXPECT_EQ(driver.vbo, 0u);
    EXPECT_EQ(driver.vaos[0].ebo, 0u);
    resource.reset();
    driver.ExpectBalanced();
}

TEST(FarVolumeGpuBackend, InvalidGeometryDoesNotEvenQueryDriver) {
    for (int invalid = 0; invalid < 6; ++invalid) {
        FakeDriver driver;
        auto mesh = Mesh();
        switch (invalid) {
            case 0:
                mesh.vertices.clear();
                break;
            case 1:
                mesh.indices.clear();
                break;
            case 2:
                mesh.indices.pop_back();
                break;
            case 3:
                mesh.indices[0] = std::uint32_t(mesh.vertices.size());
                break;
            case 4:
                mesh.content = World::FarVolumeContent::Air;
                break;
            case 5:
                mesh.content = World::FarVolumeContent::Solid;
                break;
        }
        const auto before = mesh;
        EXPECT_FALSE(UploadFarVolumeGpu(mesh, driver));
        EXPECT_EQ(driver.validity_calls, 0u);
        EXPECT_EQ(driver.error_reads, 0u);
        EXPECT_EQ(driver.operation_count, 0u);
        ExpectSameInput(mesh, before);
        driver.ExpectOldBindings();
    }
}

TEST(FarVolumeGpuBackend, PureCountChecksCoverOverflowSignedLimitsAndTriangles) {
    FarVolumeGpuSizes sizes;
    ASSERT_TRUE(CheckFarVolumeGpuSizes(4, 6, sizes));
    EXPECT_EQ(sizes.vertex_bytes, 4 * sizeof(VoxelVertex));
    EXPECT_EQ(sizes.index_bytes, 6 * sizeof(std::uint32_t));
    EXPECT_EQ(sizes.total_bytes, sizes.vertex_bytes + sizes.index_bytes);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto store_max = std::uint64_t(std::numeric_limits<std::intptr_t>::max());
    const auto count_max = std::uint64_t(std::numeric_limits<std::int32_t>::max());
    EXPECT_FALSE(CheckFarVolumeGpuSizes(0, 3, sizes));
    EXPECT_FALSE(CheckFarVolumeGpuSizes(3, 0, sizes));
    EXPECT_FALSE(CheckFarVolumeGpuSizes(3, 4, sizes));
    EXPECT_FALSE(CheckFarVolumeGpuSizes(maximum / sizeof(VoxelVertex) + 1, 3, sizes));
    EXPECT_FALSE(CheckFarVolumeGpuSizes(3, maximum, sizes));
    const auto largest_indices = maximum / sizeof(std::uint32_t);
    EXPECT_FALSE(CheckFarVolumeGpuSizes(
        maximum / sizeof(VoxelVertex), largest_indices - largest_indices % 3, sizes));
    EXPECT_FALSE(CheckFarVolumeGpuSizes(store_max / sizeof(VoxelVertex) + 1, 3, sizes));
    const auto oversized_indices = store_max / sizeof(std::uint32_t) + 1;
    EXPECT_FALSE(
        CheckFarVolumeGpuSizes(3, oversized_indices + (3 - oversized_indices % 3) % 3, sizes));
    FarVolumeGpuSizes large_vertices;
    EXPECT_EQ(CheckFarVolumeGpuSizes(count_max + 1, 3, large_vertices),
              count_max + 1 <= store_max / sizeof(VoxelVertex));
    EXPECT_FALSE(CheckFarVolumeGpuSizes(3, count_max + 2, sizes));
    // Refusal leaves the previous checked result unchanged.
    EXPECT_EQ(sizes.index_count, 6);
}

TEST(FarVolumeGpuBackend, PriorErrorIsReadOnceAndRefusesBeforeSnapshotOrCreation) {
    FakeDriver driver;
    driver.error = true;
    EXPECT_FALSE(UploadFarVolumeGpu(Mesh(), driver));
    EXPECT_EQ(driver.error_reads, 1u);
    EXPECT_EQ(driver.operation_count, 0u);
    driver.ExpectOldBindings();
}

TEST(FarVolumeGpuBackend, UnavailableContextFunctionsOrOwnerRefuseBeforeHandles) {
    for (int invalid = 0; invalid < 3; ++invalid) {
        FakeDriver driver;
        if (invalid == 0)
            driver.current_context = 2;
        if (invalid == 1)
            driver.loaded = false;
        if (invalid == 2) {
            std::thread wrong_owner([&] { EXPECT_FALSE(UploadFarVolumeGpu(Mesh(), driver)); });
            wrong_owner.join();
        } else
            EXPECT_FALSE(UploadFarVolumeGpu(Mesh(), driver));
        EXPECT_EQ(driver.error_reads, 0u);
        EXPECT_EQ(driver.operation_count, 0u);
        driver.ExpectOldBindings();
    }
}

TEST(FarVolumeGpuBackend, CleanupContinuesAfterDeletionErrorWithoutDraining) {
    FakeDriver driver;
    auto resource = UploadFarVolumeGpu(Mesh(), driver);
    ASSERT_TRUE(resource);
    const auto reads = driver.error_reads;
    driver.delete_error = true;
    resource.reset();
    EXPECT_TRUE(driver.error);
    EXPECT_EQ(driver.error_reads, reads);
    driver.ExpectOldBindings();
    driver.ExpectBalanced();
}

TEST(FarVolumeGpuBackend, WrongContextDestructionTerminatesBeforeChargeCouldRelease) {
    FakeDriver driver;
    auto resource = UploadFarVolumeGpu(Mesh(), driver);
    ASSERT_TRUE(resource);
    EXPECT_DEATH(
        {
            driver.current_context = 2;
            resource.reset();
        },
        "");
    // The parent still owns and retires its allocation on the correct context.
    resource.reset();
    driver.ExpectBalanced();
}

TEST(FarVolumeGpuBackend, WrongOwnerDestructionTerminates) {
    FakeDriver driver;
    auto resource = UploadFarVolumeGpu(Mesh(), driver);
    ASSERT_TRUE(resource);
    EXPECT_DEATH(
        {
            std::thread wrong_owner([&] { resource.reset(); });
            wrong_owner.join();
        },
        "");
    resource.reset();
    driver.ExpectBalanced();
}

TEST(FarVolumeGpuBackend, FailedIndexedSubmissionRestoresBindings) {
    for (const bool failed : {false, true}) {
        FakeDriver driver;
        auto allocation = UploadFarVolumeGpu(Mesh(), driver);
        ASSERT_TRUE(allocation);
        driver.fail_draw = failed;
        EXPECT_EQ(DrawFarVolumeGpu(*allocation, driver), !failed);
        driver.ExpectOldBindings();
    }
    FakeDriver driver;
    auto allocation = UploadFarVolumeGpu(Mesh(), driver);
    ASSERT_TRUE(allocation);
    // Snapshot, bind, draw: an actual driver error also refuses after restoration.
    driver.fail_at = driver.operation_count + 3;
    EXPECT_FALSE(DrawFarVolumeGpu(*allocation, driver));
    driver.ExpectOldBindings();
}
