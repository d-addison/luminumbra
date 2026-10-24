#include "luminumbra_client/app/FrameAttribution.h"
#include "luminumbra_client/rendering/PixelIo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Luminumbra::Client::Attribution;

TEST(FrameAttribution, RebuildReasonsIdentifyEachSignatureComponent) {
    const ScatterSignature previous{11, 22, 33, 44};
    EXPECT_EQ(classify_rebuild(previous, previous, false, false), Initial);
    EXPECT_EQ(classify_rebuild(previous, previous, false, true), Initial | Backlog);
    EXPECT_EQ(classify_rebuild(previous, previous, true, false), 0u);
    EXPECT_EQ(classify_rebuild(previous, previous, true, true), Backlog);
    EXPECT_EQ(classify_rebuild(previous, {12, 22, 33, 44}, true, false), Config);
    EXPECT_EQ(classify_rebuild(previous, {11, 23, 33, 44}, true, false), CameraCell);
    EXPECT_EQ(classify_rebuild(previous, {11, 22, 34, 44}, true, false), GroundRevision);
    EXPECT_EQ(classify_rebuild(previous, {11, 22, 33, 45}, true, false), ScatterChunks);
    EXPECT_EQ(classify_rebuild(previous, {12, 23, 34, 45}, true, true),
              Config | CameraCell | GroundRevision | ScatterChunks | Backlog);
}

TEST(FrameAttribution, CapacityMarkerIsNeverAnObservedCount) {
    constexpr std::size_t capacity = 262144;
    EXPECT_FALSE(generated_instance_count(true, false, false, capacity).has_value());
    EXPECT_FALSE(generated_instance_count(true, false, true, capacity).has_value());
    EXPECT_FALSE(generated_instance_count(true, true, false, capacity).has_value());
    const auto gpu_count = generated_instance_count(true, true, true, 123);
    ASSERT_TRUE(gpu_count.has_value());
    if (!gpu_count)
        return;
    EXPECT_EQ(gpu_count.value(), 123u);
    const auto gpu_zero = generated_instance_count(true, true, true, 0);
    ASSERT_TRUE(gpu_zero.has_value());
    if (!gpu_zero)
        return;
    EXPECT_EQ(gpu_zero.value(), 0u);
    const auto cpu_count = generated_instance_count(false, false, false, 123);
    ASSERT_TRUE(cpu_count.has_value());
    if (!cpu_count)
        return;
    EXPECT_EQ(cpu_count.value(), 123u);
    const auto cpu_zero = generated_instance_count(false, false, false, 0);
    ASSERT_TRUE(cpu_zero.has_value());
    if (!cpu_zero)
        return;
    EXPECT_EQ(cpu_zero.value(), 0u);
}

TEST(FrameAttribution, ExclusiveBucketsSumToFrameIncludingRemainder) {
    const FrameBuckets gameplay{1, 2, 3, 4, 5, 6, 7, 8, 0, 9};
    EXPECT_DOUBLE_EQ(gameplay.sum(), 45);
    EXPECT_DOUBLE_EQ(gameplay.other(50), 5);
    const FrameBuckets menu{0, 0, 0, 2, 0, 0, 0, 1, 7, 3};
    EXPECT_DOUBLE_EQ(menu.sum(), 13);
    EXPECT_DOUBLE_EQ(menu.other(15), 2);
    const std::array<FrameBuckets, 3> cases{FrameBuckets{}, gameplay, menu};
    for (const auto& buckets : cases) {
        for (const double frame : {0.0, 16.666666, 50.0, 218.0})
            EXPECT_NEAR(buckets.sum() + buckets.other(frame), frame, 1e-12);
    }
    // Preserve a negative remainder to reveal overlapping/broken scope inputs.
    EXPECT_DOUBLE_EQ(gameplay.other(44), -1);
}

std::vector<std::string> fields(const std::string& row) {
    std::vector<std::string> result;
    std::istringstream input(row);
    std::string field;
    while (std::getline(input, field, ','))
        result.push_back(field);
    return result;
}

TEST(FrameAttribution, CsvColumnsMatchHeaderAndUnknownCountsAreEmpty) {
    FoliageRebuild foliage{};
    foliage.rebuild_calls = 1;
    foliage.signature_ms = 0.125;
    foliage.sort_ms = 0.25;
    foliage.surface_ms = 0.5;
    foliage.repack_ms = 0.75;
    foliage.hash_ms = 1;
    foliage.upload_ms = 1.25;
    foliage.palette_scratch_ms = 1.5;
    foliage.dispatch_ms = 1.75;
    foliage.cpu_fallback_ms = 2;
    foliage.reasons = GroundRevision | Backlog;
    foliage.admitted_columns = 10;
    foliage.surface_misses = 4;
    foliage.deferred_columns = 6;
    foliage.copied_bytes = 1024;
    foliage.hashed_bytes = 2048;
    foliage.uploaded_bytes = 4096;
    foliage.scratch_bytes = 8192;
    foliage.dirty_columns = 2;
    foliage.worker_dispatched = 3;
    foliage.worker_completed = 2;
    foliage.worker_stale_rejected = 1;
    foliage.worker_queue_age_frames = 5;
    foliage.backlog = true;
    foliage.path = FoliagePath::Cpu;
    foliage.fallback = FallbackReason::ColumnCapacity;
    const FrameBuckets buckets{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::ostringstream header;
    write_csv_header(header);
    EXPECT_EQ(
        header.str(),
        "frame,frame_ms,sim_ms,stream_ms,render_ms,present_ms,foliage_ms,scatter_ms,"
        "rebake_ms,poll_ms,menu_ms,ui_ms,other_ms,signature_ms,sort_ms,surface_ms,"
        "repack_ms,hash_ms,upload_ms,palette_scratch_ms,dispatch_ms,cpu_fallback_ms,"
        "rebuild_calls,reason_mask,admitted_columns,surface_misses,deferred_columns,copied_bytes,"
        "hashed_bytes,uploaded_bytes,scratch_bytes,backlog,path,fallback,"
        "instance_capacity,generated_count,count_generation,"
        "terrain_work_ms,terrain_overshoot_ms,terrain_attempts,terrain_adopted,terrain_work_bytes,"
        "terrain_byte_overshoot,terrain_pending,water_work_ms,water_overshoot_ms,water_attempts,"
        "water_adopted,water_work_bytes,water_byte_overshoot,water_pending,"
        "ui_update_ms,ui_render_ms,ui_filters_ms,menu_scene_ms,preview_adoption_ms,"
        "document_load_ms,texture_load_ms,shader_load_ms,shader_first_use_ms,"
        "world_create_callback_ms,world_load_callback_ms,dirty_columns,worker_dispatched,worker_"
        "completed,worker_stale_rejected,worker_queue_age_frames\n");
    std::ostringstream unknown;
    write_csv_row(unknown, 401, 60, buckets, foliage, 262144, false, 262144, 7);
    EXPECT_EQ(unknown.str(),
              "401,60.000000,1.000000,2.000000,3.000000,4.000000,5.000000,6.000000,"
              "7.000000,8.000000,9.000000,10.000000,5.000000,0.125000,0.250000,0.500000,"
              "0.750000,1.000000,1.250000,1.500000,1.750000,2.000000,1,40,10,4,6,1024,"
              "2048,4096,8192,1,cpu,column_capacity,262144,,,"
              "0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0,0,0,0,0,"
              "0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,"
              "0.000000,0.000000,0.000000,0.000000,2,3,2,1,5\n");
    EXPECT_EQ(fields(header.str()).size(), fields(unknown.str()).size());
    std::ostringstream known;
    write_csv_row(known, 402, 60, buckets, foliage, 262144, true, 123, 7);
    EXPECT_EQ(
        known.str(),
        "402,60.000000,1.000000,2.000000,3.000000,4.000000,5.000000,6.000000,"
        "7.000000,8.000000,9.000000,10.000000,5.000000,0.125000,0.250000,0.500000,"
        "0.750000,1.000000,1.250000,1.500000,1.750000,2.000000,1,40,10,4,6,1024,"
        "2048,4096,8192,1,cpu,column_capacity,262144,123,7,"
        "0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0.000000,"
        "0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,2,3,2,1,5\n");
    EXPECT_EQ(fields(known.str()).size(), fields(header.str()).size());
    EXPECT_EQ(fields(known.str()).at(35), "123");
    EXPECT_EQ(fields(known.str()).at(36), "7");
    EXPECT_EQ(fields(known.str()).at(51 + kPhaseCount), "2");
    EXPECT_EQ(fields(known.str()).at(52 + kPhaseCount), "3");
    EXPECT_EQ(fields(known.str()).at(53 + kPhaseCount), "2");
    EXPECT_EQ(fields(known.str()).at(54 + kPhaseCount), "1");
    EXPECT_EQ(fields(known.str()).at(55 + kPhaseCount), "5\n");
    std::ostringstream zero;
    write_csv_row(zero, 403, 60, buckets, foliage, 262144, true, 0, 8);
    EXPECT_EQ(
        zero.str(),
        "403,60.000000,1.000000,2.000000,3.000000,4.000000,5.000000,6.000000,"
        "7.000000,8.000000,9.000000,10.000000,5.000000,0.125000,0.250000,0.500000,"
        "0.750000,1.000000,1.250000,1.500000,1.750000,2.000000,1,40,10,4,6,1024,"
        "2048,4096,8192,1,cpu,column_capacity,262144,0,8,"
        "0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0.000000,"
        "0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,2,3,2,1,5\n");
    EXPECT_EQ(fields(zero.str()).at(35), "0");
}

TEST(FrameAttribution, CsvRecordsShortFramesPastTheSlowFrameWindow) {
    std::ostringstream out;
    write_csv_row(out, 9000, 1, {}, {}, 262144, false, 0, 0);
    EXPECT_EQ(out.str().substr(0, 14), "9000,1.000000,");
    EXPECT_NE(out.str().find(",not_called,none,262144,,,0.000000"), std::string::npos);
    EXPECT_EQ(fields(out.str()).at(51 + kPhaseCount), "0");
}

TEST(FrameAttribution, UploadCountersAccumulateAcrossRenderAndMenuAndResetEveryFrame) {
    {
        FoliageFrameScope frame(true);
        record_render_uploads({1, 1, 500, 400, 5, 2}, false, 3);
        record_render_uploads({2, 1, 100, 0, 2, 0}, false, 1);
        record_render_uploads({1, 1, 20, 0, 1.5, 0.5}, true, 2);
        const auto& uploads = frame.snapshot().uploads;
        EXPECT_EQ(uploads.terrain.attempts, 3u);
        EXPECT_EQ(uploads.terrain.adopted, 2u);
        EXPECT_EQ(uploads.terrain.bytes, 600u);
        EXPECT_EQ(uploads.terrain.byte_overshoot, 400u);
        EXPECT_DOUBLE_EQ(uploads.terrain.elapsed_ms, 7);
        EXPECT_DOUBLE_EQ(uploads.terrain.overshoot_ms, 2);
        EXPECT_EQ(uploads.terrain_pending, 4u);
        EXPECT_EQ(uploads.water_pending, 2u);
        std::ostringstream row;
        PhaseTotals phases;
        phases.add(Phase::UiUpdate, 3, 0, 0);
        write_csv_row(row, 1, 20, {}, {}, 0, false, 0, 0, phases, uploads);
        const auto columns = fields(row.str());
        EXPECT_EQ(columns.at(37), "7.000000");
        EXPECT_EQ(columns.at(38), "2.000000");
        EXPECT_EQ(columns.at(41), "600");
        EXPECT_EQ(columns.at(42), "400");
        EXPECT_EQ(columns.at(43), "4");
        EXPECT_EQ(columns.at(44), "1.500000");
        EXPECT_EQ(columns.at(50), "2");
        EXPECT_EQ(columns.at(51), "3.000000");
        EXPECT_EQ(columns.size(), 56u + kPhaseCount);
    }
    record_render_uploads({1, 1, 10, 0, 1, 0}, false, 10);
    FoliageFrameScope next(true);
    EXPECT_EQ(next.snapshot().uploads.terrain.attempts, 0u);
    EXPECT_EQ(next.snapshot().uploads.terrain_pending, 0u);
}

TEST(FrameAttribution, NestedRebuildsAreRemovedFromTheirParentBuckets) {
    FrameBuckets buckets{};
    buckets.render = exclusive_parent_ms(20, 3, 8);
    buckets.menu = exclusive_parent_ms(12, 8, 15);
    buckets.foliage = 15; // 3 outside parents, 5 inside render, 7 inside menu.
    buckets.present = 2;
    EXPECT_DOUBLE_EQ(buckets.render, 15);
    EXPECT_DOUBLE_EQ(buckets.menu, 5);
    EXPECT_DOUBLE_EQ(buckets.sum(), 37);
    EXPECT_DOUBLE_EQ(buckets.other(40), 3);
}

TEST(FrameAttribution, FrameCollectorIncludesAllCallersAndDoesNotLeakBetweenFrames) {
    EXPECT_FALSE(foliage_frame_active());
    FoliageRebuild gpu{};
    gpu.path = FoliagePath::Gpu;
    gpu.reasons = Initial;
    gpu.signature_ms = 1;
    gpu.sort_ms = 2;
    gpu.surface_ms = 3;
    gpu.repack_ms = 4;
    gpu.hash_ms = 5;
    gpu.upload_ms = 6;
    gpu.palette_scratch_ms = 7;
    gpu.dispatch_ms = 8;
    gpu.admitted_columns = 5;
    gpu.surface_misses = 4;
    gpu.surface_samples = 32768;
    gpu.deferred_columns = 1;
    gpu.copied_bytes = 100;
    gpu.hashed_bytes = 200;
    gpu.uploaded_bytes = 300;
    gpu.uploads = 4;
    gpu.stale_results = 1;
    gpu.scratch_bytes = 400;
    gpu.dirty_columns = 2;
    gpu.worker_dispatched = 4;
    gpu.worker_completed = 3;
    gpu.worker_stale_rejected = 1;
    gpu.worker_queue_age_frames = 7;
    gpu.backlog = true;
    {
        FoliageFrameScope frame(true);
        EXPECT_TRUE(foliage_frame_active());
        record_foliage_rebuild(gpu, 40, 262144, std::nullopt, 0);
        record_foliage_rebuild(gpu, 40, 262144, 123, 7);
        const auto& first = frame.snapshot();
        EXPECT_EQ(first.rebuild.rebuild_calls, 2u);
        EXPECT_EQ(first.rebuild.path, FoliagePath::Gpu);
        EXPECT_EQ(first.rebuild.fallback, FallbackReason::None);
        EXPECT_EQ(first.rebuild.reasons, Initial);
        EXPECT_DOUBLE_EQ(first.rebuild.signature_ms, 2);
        EXPECT_DOUBLE_EQ(first.rebuild.sort_ms, 4);
        EXPECT_DOUBLE_EQ(first.rebuild.surface_ms, 6);
        EXPECT_DOUBLE_EQ(first.rebuild.repack_ms, 8);
        EXPECT_DOUBLE_EQ(first.rebuild.hash_ms, 10);
        EXPECT_DOUBLE_EQ(first.rebuild.upload_ms, 12);
        EXPECT_DOUBLE_EQ(first.rebuild.palette_scratch_ms, 14);
        EXPECT_DOUBLE_EQ(first.rebuild.dispatch_ms, 16);
        EXPECT_EQ(first.rebuild.admitted_columns, 10u);
        EXPECT_EQ(first.rebuild.surface_misses, 8u);
        EXPECT_EQ(first.rebuild.surface_samples, 65536u);
        EXPECT_EQ(first.rebuild.deferred_columns, 2u);
        EXPECT_EQ(first.rebuild.copied_bytes, 200u);
        EXPECT_EQ(first.rebuild.hashed_bytes, 400u);
        EXPECT_EQ(first.rebuild.uploaded_bytes, 600u);
        EXPECT_EQ(first.rebuild.uploads, 8u);
        EXPECT_EQ(first.rebuild.stale_results, 2u);
        EXPECT_EQ(first.rebuild.scratch_bytes, 800u);
        EXPECT_EQ(first.rebuild.dirty_columns, 4u);
        EXPECT_EQ(first.rebuild.worker_dispatched, 8u);
        EXPECT_EQ(first.rebuild.worker_completed, 6u);
        EXPECT_EQ(first.rebuild.worker_stale_rejected, 2u);
        EXPECT_EQ(first.rebuild.worker_queue_age_frames, 7u);
        EXPECT_TRUE(first.rebuild.backlog);
        EXPECT_DOUBLE_EQ(first.rebuild_ms, 80);
        EXPECT_TRUE(first.count_known);
        EXPECT_EQ(first.generated_count, 123u);
        EXPECT_EQ(first.count_generation, 7u);
        {
            FoliageFrameScope nested_frame(true);
            record_foliage_rebuild(gpu, 40, 262144, 1, 8);
            EXPECT_EQ(nested_frame.snapshot().rebuild.rebuild_calls, 1u);
            EXPECT_EQ(frame.snapshot().rebuild.rebuild_calls, 2u);
        }
        FoliageRebuild cpu{};
        cpu.path = FoliagePath::Cpu;
        cpu.fallback = FallbackReason::ColumnCapacity;
        cpu.reasons = Backlog;
        cpu.cpu_fallback_ms = 10;
        record_foliage_rebuild(cpu, 11, 262144, std::nullopt, 0);
        EXPECT_EQ(frame.snapshot().rebuild.rebuild_calls, 3u);
        EXPECT_EQ(frame.snapshot().rebuild.path, FoliagePath::Mixed);
        EXPECT_EQ(frame.snapshot().rebuild.fallback, FallbackReason::Mixed);
        EXPECT_EQ(frame.snapshot().rebuild.reasons, Initial | Backlog);
        EXPECT_DOUBLE_EQ(frame.snapshot().rebuild.cpu_fallback_ms, 10);
        EXPECT_DOUBLE_EQ(frame.snapshot().rebuild_ms, 91);
        EXPECT_FALSE(frame.snapshot().count_known);
    }
    EXPECT_FALSE(foliage_frame_active());
    record_foliage_rebuild(gpu, 40, 262144, 123, 7);
    FoliageFrameScope next_frame(true);
    EXPECT_EQ(next_frame.snapshot().rebuild.rebuild_calls, 0u);
    EXPECT_EQ(next_frame.snapshot().rebuild.path, FoliagePath::NotCalled);
    EXPECT_DOUBLE_EQ(next_frame.snapshot().rebuild_ms, 0);
}

TEST(FrameAttribution, FrameCollectorsAreIsolatedAcrossThreads) {
    FoliageFrameScope frame(true);
    record_foliage_rebuild({}, 10, 262144, 123, 7);
    record_render_uploads({1, 1, 10, 0, 1, 0}, false, 2);
    // The parent stays idle until join so a shared-pointer regression fails
    // deterministically without racing accesses to either frame.
    bool worker_initially_active = false;
    bool worker_scope_active = false;
    bool worker_finally_active = false;
    FoliageFrame worker_snapshot{};
    std::thread worker([&] {
        worker_initially_active = foliage_frame_active();
        record_foliage_rebuild({}, 20, 262144, 456, 8);
        record_render_uploads({2, 1, 20, 0, 2, 0}, false, 3);
        {
            FoliageFrameScope worker_frame(true);
            worker_scope_active = foliage_frame_active();
            record_foliage_rebuild({}, 30, 262144, 789, 9);
            record_render_uploads({3, 2, 30, 0, 3, 0}, true, 4);
            worker_snapshot = worker_frame.snapshot();
        }
        worker_finally_active = foliage_frame_active();
    });
    worker.join();
    EXPECT_FALSE(worker_initially_active);
    EXPECT_TRUE(worker_scope_active);
    EXPECT_FALSE(worker_finally_active);
    EXPECT_EQ(worker_snapshot.rebuild.rebuild_calls, 1u);
    EXPECT_EQ(worker_snapshot.generated_count, 789u);
    EXPECT_EQ(worker_snapshot.uploads.terrain.attempts, 0u);
    EXPECT_EQ(worker_snapshot.uploads.water.attempts, 3u);
    EXPECT_EQ(worker_snapshot.uploads.water_pending, 4u);
    EXPECT_TRUE(foliage_frame_active());
    EXPECT_EQ(frame.snapshot().rebuild.rebuild_calls, 1u);
    EXPECT_EQ(frame.snapshot().generated_count, 123u);
    EXPECT_EQ(frame.snapshot().uploads.terrain.attempts, 1u);
    EXPECT_EQ(frame.snapshot().uploads.terrain_pending, 2u);
    EXPECT_EQ(frame.snapshot().uploads.water.attempts, 0u);
    record_foliage_rebuild({}, 40, 262144, 321, 10);
    EXPECT_EQ(frame.snapshot().rebuild.rebuild_calls, 2u);
    EXPECT_EQ(frame.snapshot().generated_count, 321u);
}

TEST(FrameAttribution, DisabledCollectionDoesNotPublishOrChangeStageTotals) {
    double milliseconds = 7;
    {
        StageTimer timer(milliseconds, false);
        timer.stop();
        timer.stop();
    }
    EXPECT_DOUBLE_EQ(milliseconds, 7);
    FoliageFrameScope disabled;
    EXPECT_FALSE(foliage_frame_active());
    record_foliage_rebuild({}, 10, 262144, 123, 7);
    record_render_uploads({1, 1, 10, 0, 1, 0}, false, 2);
    EXPECT_EQ(disabled.snapshot().uploads.terrain.attempts, 0u);
    EXPECT_EQ(disabled.snapshot().rebuild.rebuild_calls, 0u);
    EXPECT_DOUBLE_EQ(disabled.snapshot().rebuild_ms, 0);
    {
        FoliageFrameScope enabled(true);
        ASSERT_TRUE(foliage_frame_active());
        {
            FoliageFrameScope nested_disabled(false);
            EXPECT_FALSE(foliage_frame_active());
            record_foliage_rebuild({}, 10, 262144, 123, 7);
            record_render_uploads({1, 1, 10, 0, 1, 0}, false, 2);
        }
        EXPECT_TRUE(foliage_frame_active());
        EXPECT_EQ(enabled.snapshot().rebuild.rebuild_calls, 0u);
        EXPECT_EQ(enabled.snapshot().uploads.terrain.attempts, 0u);
        record_foliage_rebuild({}, 10, 262144, 123, 7);
        record_render_uploads({1, 1, 10, 0, 1, 0}, false, 2);
        EXPECT_EQ(enabled.snapshot().rebuild.rebuild_calls, 1u);
        EXPECT_EQ(enabled.snapshot().uploads.terrain.attempts, 1u);
    }
    EXPECT_FALSE(foliage_frame_active());
}

TEST(FrameAttribution, ConstScopesCollectObservationsAndRestoreDisabledParents) {
    EXPECT_FALSE(foliage_frame_active());
    record_foliage_rebuild({}, 99, 262144, 99, 99);
    {
        const FoliageFrameScope disabled;
        EXPECT_FALSE(foliage_frame_active());
        {
            const FoliageFrameScope frame(true);
            const auto& snapshot = frame.snapshot();
            EXPECT_TRUE(foliage_frame_active());
            EXPECT_EQ(snapshot.rebuild.rebuild_calls, 0u);
            EXPECT_FALSE(snapshot.count_known);
            FoliageRebuild gpu{};
            gpu.path = FoliagePath::Gpu;
            gpu.reasons = Initial;
            gpu.hashed_bytes = 200;
            gpu.uploaded_bytes = 300;
            record_foliage_rebuild(gpu, 4, 262144, 123, 7);
            EXPECT_EQ(snapshot.rebuild.rebuild_calls, 1u);
            EXPECT_EQ(snapshot.rebuild.hashed_bytes, 200u);
            EXPECT_EQ(snapshot.rebuild.uploaded_bytes, 300u);
            EXPECT_EQ(snapshot.capacity, 262144u);
            EXPECT_TRUE(snapshot.count_known);
            EXPECT_EQ(snapshot.generated_count, 123u);
            EXPECT_EQ(snapshot.count_generation, 7u);
            {
                const FoliageFrameScope nested_disabled(false);
                EXPECT_FALSE(foliage_frame_active());
                record_foliage_rebuild(gpu, 99, 262144, 99, 99);
                {
                    const FoliageFrameScope nested_enabled(true);
                    EXPECT_TRUE(foliage_frame_active());
                    record_foliage_rebuild(gpu, 2, 262144, 0, 8);
                    EXPECT_EQ(nested_enabled.snapshot().rebuild.rebuild_calls, 1u);
                    EXPECT_TRUE(nested_enabled.snapshot().count_known);
                    EXPECT_EQ(nested_enabled.snapshot().generated_count, 0u);
                    EXPECT_EQ(nested_enabled.snapshot().count_generation, 8u);
                    EXPECT_EQ(snapshot.rebuild.rebuild_calls, 1u);
                }
                EXPECT_FALSE(foliage_frame_active());
                record_foliage_rebuild(gpu, 99, 262144, 99, 99);
                EXPECT_EQ(nested_disabled.snapshot().rebuild.rebuild_calls, 0u);
                EXPECT_DOUBLE_EQ(nested_disabled.snapshot().rebuild_ms, 0);
            }
            EXPECT_TRUE(foliage_frame_active());
            EXPECT_EQ(snapshot.rebuild.rebuild_calls, 1u);
            FoliageRebuild cpu{};
            cpu.path = FoliagePath::Cpu;
            cpu.reasons = Config;
            cpu.uploaded_bytes = 50;
            record_foliage_rebuild(cpu, 3, 128, std::nullopt, 0);
            EXPECT_EQ(&snapshot, &frame.snapshot());
            EXPECT_EQ(snapshot.rebuild.rebuild_calls, 2u);
            EXPECT_EQ(snapshot.rebuild.path, FoliagePath::Mixed);
            EXPECT_EQ(snapshot.rebuild.reasons, Initial | Config);
            EXPECT_EQ(snapshot.rebuild.hashed_bytes, 200u);
            EXPECT_EQ(snapshot.rebuild.uploaded_bytes, 350u);
            EXPECT_DOUBLE_EQ(snapshot.rebuild_ms, 7);
            EXPECT_EQ(snapshot.capacity, 128u);
            EXPECT_FALSE(snapshot.count_known);
            EXPECT_EQ(snapshot.generated_count, 0u);
            EXPECT_EQ(snapshot.count_generation, 0u);
        }
        EXPECT_FALSE(foliage_frame_active());
        record_foliage_rebuild({}, 99, 262144, 99, 99);
        EXPECT_EQ(disabled.snapshot().rebuild.rebuild_calls, 0u);
        EXPECT_DOUBLE_EQ(disabled.snapshot().rebuild_ms, 0);
    }
    EXPECT_FALSE(foliage_frame_active());
    record_foliage_rebuild({}, 99, 262144, 99, 99);
    const FoliageFrameScope next_frame(true);
    EXPECT_EQ(next_frame.snapshot().rebuild.rebuild_calls, 0u);
    EXPECT_EQ(next_frame.snapshot().rebuild.path, FoliagePath::NotCalled);
    EXPECT_FALSE(next_frame.snapshot().count_known);
    EXPECT_DOUBLE_EQ(next_frame.snapshot().rebuild_ms, 0);
}

TEST(FrameAttribution, ConstCollectorsOwnMutableTotalsAndResetTheNextFrame) {
    {
        const FoliageFrameScope foliage(true);
        const PhaseFrameScope phases(true);
        {
            const PhaseScope scene(Phase::MenuScene);
            record_foliage_rebuild({}, 0, 262144, std::nullopt, 0);
            EXPECT_TRUE(scene.is_current());
        }
        EXPECT_EQ(phases.snapshot().completed_scopes(Phase::MenuScene), 1u);
        EXPECT_EQ(foliage.snapshot().rebuild.rebuild_calls, 1u);
        EXPECT_EQ(foliage.snapshot().capacity, 262144u);
        EXPECT_FALSE(foliage.snapshot().count_known);
        EXPECT_EQ(foliage.snapshot().generated_count, 0u);
        EXPECT_EQ(foliage.snapshot().count_generation, 0u);
    }
    EXPECT_FALSE(foliage_frame_active());
    EXPECT_DOUBLE_EQ(phase_total_ms(), 0);
    const FoliageFrameScope next_foliage(true);
    const PhaseFrameScope next_phases(true);
    EXPECT_EQ(next_foliage.snapshot().rebuild.rebuild_calls, 0u);
    EXPECT_EQ(next_phases.snapshot().completed_scopes(Phase::MenuScene), 0u);
}

TEST(FrameAttribution, PhaseCollectorsAreIsolatedAcrossThreads) {
    const PhaseFrameScope frame(true);
    const PhaseScope parent(Phase::MenuScene);
    bool worker_saw_parent = false;
    bool worker_scope_active = false;
    bool worker_frame_active = false;
    bool worker_disabled_active = false;
    bool worker_restored = false;
    bool worker_finally_saw_parent = false;
    double worker_initial_total = -1;
    double worker_final_total = -1;
    PhaseTotals worker_totals;
    // The parent stays idle until join: shared-state mutations fail without
    // concurrent reads/writes or assumptions about scheduler/clock precision.
    std::thread worker([&] {
        worker_saw_parent = parent.is_current();
        worker_initial_total = phase_total_ms();
        {
            const PhaseFrameScope worker_frame(true);
            worker_frame_active = worker_frame.is_current();
            {
                const PhaseScope ui(Phase::UiUpdate);
                worker_scope_active = ui.is_current();
                {
                    const PhaseFrameScope disabled(false);
                    const PhaseScope ignored(Phase::UiFilters);
                    worker_disabled_active = disabled.is_current() || ignored.is_current();
                }
                worker_restored = worker_frame.is_current() && ui.is_current();
            }
            worker_totals = worker_frame.snapshot();
        }
        worker_finally_saw_parent = parent.is_current();
        worker_final_total = phase_total_ms();
    });
    worker.join();
    EXPECT_FALSE(worker_saw_parent);
    EXPECT_DOUBLE_EQ(worker_initial_total, 0);
    EXPECT_TRUE(worker_frame_active);
    EXPECT_TRUE(worker_scope_active);
    EXPECT_FALSE(worker_disabled_active);
    EXPECT_TRUE(worker_restored);
    EXPECT_FALSE(worker_finally_saw_parent);
    EXPECT_DOUBLE_EQ(worker_final_total, 0);
    EXPECT_EQ(worker_totals.completed_scopes(Phase::UiUpdate), 1u);
    EXPECT_EQ(worker_totals.completed_scopes(Phase::UiFilters), 0u);
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::UiUpdate), 0u);
    EXPECT_TRUE(frame.is_current());
    EXPECT_TRUE(parent.is_current());
    {
        const PhaseScope ui(Phase::UiUpdate);
        EXPECT_TRUE(ui.is_current());
    }
    EXPECT_TRUE(parent.is_current());
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::UiUpdate), 1u);
}

// A bounded sink that accepts a prefix, then reports a write error on every
// platform. It exercises the same row writer and status used by FrameRecorder.
class FailingBuffer : public std::streambuf {
public:
    std::streamsize written() const {
        return m_written;
    }

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        const auto accepted = std::min(count, kCapacity - m_written);
        m_written += accepted;
        return accepted;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        if (m_written == kCapacity)
            return traits_type::eof();
        ++m_written;
        return value;
    }

private:
    // Accept at most 64 bytes without retaining any data.
    static constexpr std::streamsize kCapacity = 64;
    std::streamsize m_written = 0;
};

TEST(FrameAttribution, PartialWriteFailureIsReportedAndRemainsInactive) {
    FailingBuffer buffer;
    std::ostream out(&buffer);
    EXPECT_FALSE(write_csv_row(out, 1, 1, {}, {}, 262144, false, 0, 0));
    EXPECT_FALSE(out.good());
    EXPECT_EQ(buffer.written(), 64);
    EXPECT_FALSE(write_csv_row(out, 2, 1, {}, {}, 262144, false, 0, 0));
    EXPECT_EQ(buffer.written(), 64);
}

// Accept writes without retaining data, but fail only when explicitly flushed.
class FlushFailingBuffer : public std::streambuf {
protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        return count;
    }
    int_type overflow(int_type value) override {
        return traits_type::not_eof(value);
    }
    int sync() override {
        return -1;
    }
};

TEST(CaptureArtifacts, TruncatedIndexFlushFailureAndCompleteStreamAreDistinct) {
    using namespace Luminumbra::Rendering;
    FailingBuffer short_output;
    std::ostream truncated(&short_output);
    EXPECT_FALSE(WriteCaptureStream(truncated, std::string(80, ' ') + "[]\n"));
    EXPECT_FALSE(truncated.good());
    FlushFailingBuffer flush_output;
    std::ostream flush_failure(&flush_output);
    EXPECT_FALSE(WriteCaptureStream(flush_failure, "[]\n"));
    std::ofstream unopened;
    EXPECT_FALSE(CloseCaptureFile(unopened));
    std::ostringstream complete;
    EXPECT_TRUE(WriteCaptureStream(complete, "[]\n"));
    EXPECT_EQ(complete.str(), "[]\n");
    std::size_t bytes = 19;
    EXPECT_FALSE(CapturePixelBytes(0, 1, 3, bytes));
    EXPECT_FALSE(CapturePixelBytes(1, -1, 3, bytes));
    EXPECT_FALSE(CapturePixelBytes(1, 1, 0, bytes));
    EXPECT_FALSE(CapturePixelBytes(1, 1, std::numeric_limits<std::size_t>::max(), bytes));
    EXPECT_FALSE(CapturePixelBytes(std::numeric_limits<int>::max(), 2, 4, bytes));
    EXPECT_EQ(bytes, 19u); // refusal leaves the caller's allocation size untouched
    EXPECT_TRUE(CapturePixelBytes(2, 3, 3, bytes));
    EXPECT_EQ(bytes, 18u);
}

class FrameRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Atomic directory creation isolates concurrently launched CTest cases.
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                                   ("luminumbra-frame-attribution-" + std::to_string(stamp) + "-" +
                                    std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                root = candidate;
                break;
            }
        }
        ASSERT_FALSE(root.empty());
    }
    void TearDown() override {
        if (!root.empty())
            std::filesystem::remove_all(root);
    }
    std::filesystem::path root;
};

TEST_F(FrameRecorderTest, DisabledRecorderCreatesNoLogs) {
    FrameRecorder recorder(false, root / "logs");
    EXPECT_FALSE(recorder.active());
    EXPECT_FALSE(std::filesystem::exists(root / "logs"));
    EXPECT_TRUE(recorder.finish());
    EXPECT_TRUE(recorder.finish());
    EXPECT_FALSE(recorder.record(1, 1, {}, {}, 0, false, 0, 0));
}

TEST_F(FrameRecorderTest, DirectoryCreationFailureLeavesRecorderInactive) {
    const auto logs = root / "logs";
    {
        std::ofstream blocker(logs, std::ios::binary);
        ASSERT_TRUE(blocker.good());
    }
    FrameRecorder recorder(true, logs);
    EXPECT_FALSE(recorder.active());
    EXPECT_FALSE(recorder.record(1, 1, {}, {}, 0, false, 0, 0));
    EXPECT_TRUE(std::filesystem::is_regular_file(logs));
    EXPECT_FALSE(recorder.finish());
    EXPECT_FALSE(recorder.finish());
}

TEST_F(FrameRecorderTest, FileOpenFailureLeavesRecorderInactive) {
    const auto destination = root / "logs/frame-attribution.csv";
    ASSERT_TRUE(std::filesystem::create_directories(destination));
    FrameRecorder recorder(true, root / "logs");
    EXPECT_FALSE(recorder.active());
    EXPECT_FALSE(recorder.record(1, 1, {}, {}, 0, false, 0, 0));
    EXPECT_TRUE(std::filesystem::is_directory(destination));
    EXPECT_FALSE(recorder.finish());
    EXPECT_FALSE(recorder.finish());
}

TEST_F(FrameRecorderTest, FinalFlushFailureIsReportedAfterSuccessfulWrites) {
    FlushFailingBuffer buffer;
    std::ofstream out(root / "frame-attribution.csv", std::ios::binary);
    ASSERT_TRUE(out.is_open());
    // Replace only the ostream buffer; close still releases the owned file.
    static_cast<std::ostream&>(out).rdbuf(&buffer);
    write_csv_header(out);
    ASSERT_TRUE(write_csv_row(out, 1, 1, {}, {}, 262144, false, 0, 0));
    ASSERT_TRUE(out.good());
    EXPECT_FALSE(finalize_csv(out));
    EXPECT_FALSE(out.is_open());
    EXPECT_FALSE(out.good());
    EXPECT_FALSE(write_csv_row(out, 2, 1, {}, {}, 262144, false, 0, 0));
}

TEST_F(FrameRecorderTest, FinalizationPreservesEarlierWriteFailure) {
    FailingBuffer buffer;
    std::ofstream out(root / "frame-attribution.csv", std::ios::binary);
    ASSERT_TRUE(out.is_open());
    static_cast<std::ostream&>(out).rdbuf(&buffer);
    ASSERT_FALSE(write_csv_row(out, 1, 1, {}, {}, 262144, false, 0, 0));
    EXPECT_FALSE(finalize_csv(out));
    EXPECT_FALSE(out.is_open());
    EXPECT_FALSE(out.good());
}

TEST_F(FrameRecorderTest, RecordsEveryShortFrameAndDrainsBufferOnClose) {
    {
        FrameRecorder recorder(true, root / "logs");
        ASSERT_TRUE(recorder.active());
        for (std::uint64_t frame = 1; frame <= 501; ++frame) {
            // Exercise unavailable, observed zero and observed nonzero counts
            // across buffer flushes. Capacity is never a generated count.
            const bool known = frame % 3 != 0;
            const auto count = frame % 3 == 1 ? 0u : 123u;
            FoliageRebuild foliage{};
            foliage.dirty_columns = frame;
            ASSERT_TRUE(recorder.record(
                frame, 1, {}, foliage, 262144, known, known ? count : 262144, frame));
        }
        EXPECT_TRUE(recorder.finish());
        EXPECT_FALSE(recorder.active());
        EXPECT_FALSE(recorder.record(502, 1, {}, {}, 262144, false, 262144, 0));
        EXPECT_TRUE(recorder.finish());
    }
    // Close the writer before reading/removing files, including on Windows.
    std::ifstream input(root / "logs/frame-attribution.csv", std::ios::binary);
    ASSERT_TRUE(input.good());
    std::string line;
    ASSERT_TRUE(std::getline(input, line));
    const auto header = fields(line);
    ASSERT_EQ(header.size(), 56u + kPhaseCount);
    EXPECT_EQ(header.at(34), "instance_capacity");
    EXPECT_EQ(header.at(35), "generated_count");
    EXPECT_EQ(header.at(36), "count_generation");
    EXPECT_EQ(
        line,
        "frame,frame_ms,sim_ms,stream_ms,render_ms,present_ms,foliage_ms,scatter_ms,"
        "rebake_ms,poll_ms,menu_ms,ui_ms,other_ms,signature_ms,sort_ms,surface_ms,"
        "repack_ms,hash_ms,upload_ms,palette_scratch_ms,dispatch_ms,cpu_fallback_ms,"
        "rebuild_calls,reason_mask,admitted_columns,surface_misses,deferred_columns,copied_bytes,"
        "hashed_bytes,uploaded_bytes,scratch_bytes,backlog,path,fallback,"
        "instance_capacity,generated_count,count_generation,"
        "terrain_work_ms,terrain_overshoot_ms,terrain_attempts,terrain_adopted,terrain_work_bytes,"
        "terrain_byte_overshoot,terrain_pending,water_work_ms,water_overshoot_ms,water_attempts,"
        "water_adopted,water_work_bytes,water_byte_overshoot,water_pending,"
        "ui_update_ms,ui_render_ms,ui_filters_ms,menu_scene_ms,preview_adoption_ms,"
        "document_load_ms,texture_load_ms,shader_load_ms,shader_first_use_ms,"
        "world_create_callback_ms,world_load_callback_ms,dirty_columns,worker_dispatched,worker_"
        "completed,worker_stale_rejected,worker_queue_age_frames");
    EXPECT_EQ(header.at(51 + kPhaseCount), "dirty_columns");
    std::string last;
    std::size_t rows = 1;
    while (std::getline(input, line)) {
        // Pin every existing value and field at its original index, with the
        // dirty-column and worker fields appended, for every buffered short frame.
        EXPECT_EQ(
            line,
            std::to_string(rows) +
                ",1.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,"
                "0.000000,0.000000,0.000000,0.000000,1.000000,0.000000,0.000000,"
                "0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,"
                "0,0,0,0,0,0,0,0,0,0,not_called,none,262144," +
                (rows % 3 == 0   ? std::string{}
                 : rows % 3 == 1 ? "0"
                                 : "123") +
                "," + (rows % 3 == 0 ? std::string{} : std::to_string(rows)) + "," +
                "0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0,0,0,0,0,0.000000,0.000000,0."
                "000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000," +
                std::to_string(rows) + ",0,0,0,0");
        EXPECT_EQ(fields(line).size(), 56u + kPhaseCount);
        last = line;
        const auto columns = fields(line);
        ASSERT_EQ(columns.size(), header.size());
        EXPECT_EQ(columns.at(0), std::to_string(rows));
        EXPECT_EQ(columns.at(1), "1.000000");
        EXPECT_EQ(columns.at(34), "262144");
        EXPECT_EQ(columns.at(35), rows % 3 == 0 ? "" : rows % 3 == 1 ? "0" : "123");
        EXPECT_EQ(columns.at(36), rows % 3 == 0 ? "" : std::to_string(rows));
        // Upload fields follow the nullable counts; all fourteen must retain
        // their exact zero values and time precision on every buffered row.
        for (std::size_t column = 37; column < 51; ++column) {
            SCOPED_TRACE(header.at(column));
            const bool time = column == 37 || column == 38 || column == 44 || column == 45;
            EXPECT_EQ(columns.at(column), time ? "0.000000" : "0");
        }
        EXPECT_EQ(columns.at(51 + kPhaseCount), std::to_string(rows));
        for (std::size_t index = 51; index < 51 + kPhaseCount; ++index)
            EXPECT_EQ(columns.at(index), "0.000000");
        for (std::size_t index = 52 + kPhaseCount; index < 56 + kPhaseCount; ++index)
            EXPECT_EQ(columns.at(index), "0");
        ++rows;
    }
    EXPECT_TRUE(input.eof());
    EXPECT_FALSE(input.bad());
    ASSERT_EQ(rows, 502u); // Header and every frame, including beyond 400.
    EXPECT_TRUE(input.eof());
    EXPECT_EQ(last.substr(0, 13), "501,1.000000,");
    EXPECT_EQ(fields(last).at(34), "262144");
    EXPECT_EQ(fields(last).at(35), "");
    EXPECT_EQ(fields(last).at(36), "");
    EXPECT_EQ(fields(last).at(51 + kPhaseCount), "501");
    for (std::size_t index = 51; index < 51 + kPhaseCount; ++index)
        EXPECT_EQ(fields(last).at(index), "0.000000");
}
} // namespace
