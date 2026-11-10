#include "luminumbra_client/ui/GraphicsSettingsState.h"
#include "luminumbra_client/ui/TgaTexture.h"
#include "luminumbra_common/core/SystemConfig.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace Luminumbra::Client;
using namespace luminumbra::core;

GraphicsEdit Preset(GraphicsPreset preset) {
    GraphicsEdit edit;
    edit.kind = GraphicsEditKind::Preset;
    edit.preset = preset;
    return edit;
}

GraphicsEdit Override(std::string key, GraphicsOverrideValue value) {
    GraphicsEdit edit;
    edit.kind = GraphicsEditKind::Override;
    edit.key = std::move(key);
    edit.value = std::move(value);
    return edit;
}

TEST(GraphicsSettingsState, CustomRetainsBaseAndResetRestoresIt) {
    GraphicsSettings settings;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::High));
    settings.overrides["grass_density"] = 1.6;
    EXPECT_EQ(BuildGraphicsSettingsDisplay(settings, {}, "").preset_label, "High");
    settings.overrides["grass_density"] = 0.6;
    GraphicsSettingsState state(settings, {});
    EXPECT_EQ(state.Display().preset_label, "Custom (base: High)");
    ASSERT_TRUE(state.Stage(GraphicsEdit{}));
    const auto& pending = state.Pending();
    if (!pending.has_value()) {
        ADD_FAILURE() << "Staging the reset must queue graphics settings";
        return;
    }
    EXPECT_TRUE(pending->overrides.empty());
    EXPECT_EQ(state.Settings().overrides, settings.overrides); // No commit before renderer success.
    state.Applied(true, 0.0);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::High);
    EXPECT_EQ(state.Display().preset_label, "High");
    EXPECT_DOUBLE_EQ(state.Display().effective.values.grass_density, 1.6);
}

TEST(GraphicsSettingsState, SelectingEachPresetClearsEveryOverride) {
    for (const auto preset : {GraphicsPreset::Low,
                              GraphicsPreset::Medium,
                              GraphicsPreset::High,
                              GraphicsPreset::Ultra}) {
        GraphicsSettings settings;
        settings.overrides = {
            {"render_scale", 0.5}, {"grass_enabled", false}, {"ui_blur", std::string("off")}};
        GraphicsSettingsState state(settings, {});
        ASSERT_TRUE(state.Stage(Preset(preset)));
        state.Applied(true, 0.0);
        EXPECT_EQ(state.Settings().base_preset, preset);
        EXPECT_TRUE(state.Settings().overrides.empty());
        EXPECT_FALSE(state.Display().effective.custom);
        EXPECT_EQ(state.Settings().preset_revision, kGraphicsPresetRevision);
        EXPECT_EQ(state.Display().effective.values, GraphicsValues{});
    }
}

TEST(GraphicsSettingsState, DetectionIsRepeatableAndPreservesConservativeVisualsForEveryCpuTier) {
    GraphicsHardwareInputs inputs;
    inputs.vendor = "NVIDIA";
    inputs.renderer = "NVIDIA GeForce RTX 5070 Ti";
    inputs.version = "4.6";
    inputs.dedicatedVideoMemoryBytes = 8ULL * 1024 * 1024 * 1024;
    GraphicsEdit detect;
    detect.kind = GraphicsEditKind::DetectApply;
    constexpr unsigned int counts[]{4, 7, 15, 16};
    constexpr GraphicsPreset presets[]{
        GraphicsPreset::Low, GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra};
    for (std::size_t index = 0; index < std::size(counts); ++index) {
        inputs.logicalProcessorCount = counts[index];
        const auto recommendation = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(recommendation, ClassifyGraphicsPreset(inputs));
        EXPECT_EQ(recommendation.preset, presets[index]);
        GraphicsSettingsState state({}, {});
        ASSERT_TRUE(state.Stage(detect, inputs));
        EXPECT_FALSE(state.SavePending());
        state.Applied(true, 1.0);
        EXPECT_EQ(state.Settings().base_preset, presets[index]);
        EXPECT_EQ(state.Settings().preset_revision, kGraphicsPresetRevision);
        EXPECT_EQ(state.Display().effective.values, GraphicsValues{});
        EXPECT_TRUE(state.SaveDue(1.5));
        EXPECT_FALSE(state.Display().hardware.empty());
    }
}

TEST(GraphicsSettingsState, InvalidRequestsDoNotReplaceAnAlreadyQueuedTransaction) {
    GraphicsSettingsState state({}, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Low)));
    for (const auto& edit :
         {Override("render_scale", 1.01),
          Override("grass_density", 0.0),
          Override("grass_fade_start_m", 92.0), // Conservative presets end at 92.
          Override("grass_fade_end_m", 48.0),
          Override("far_terrain_radius_m", 767.0),
          Override("shadow_resolution", 512.0),
          Override("volumetric_quality", 2.0),
          Override("ui_blur", std::string("invalid")),
          Override("render_scale", std::numeric_limits<double>::quiet_NaN()),
          Override("water_resolution", 8.0),
          Preset(static_cast<GraphicsPreset>(255))}) {
        EXPECT_FALSE(state.Stage(edit));
        const auto& pending = state.Pending();
        if (!pending.has_value()) {
            ADD_FAILURE() << "An invalid edit must retain the queued graphics settings";
            return;
        }
        EXPECT_EQ(pending->base_preset, GraphicsPreset::Low);
        EXPECT_TRUE(pending->overrides.empty());
        EXPECT_FALSE(state.Display().status.empty());
    }
}

TEST(GraphicsSettingsState, RapidEditsCoalesceAndSaveOnlyAfterSuccessfulApplyAndDebounce) {
    GraphicsSettingsState state({}, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Medium)));
    ASSERT_TRUE(state.Stage(Override("grass_density", 0.5)));
    EXPECT_FALSE(state.SaveDue(100.0));
    state.Applied(true, 1.0);
    EXPECT_FALSE(state.SaveDue(1.499));
    ASSERT_TRUE(state.Stage(Override("ui_blur", std::string("off"))));
    EXPECT_FALSE(state.SaveDue(100.0));
    state.Applied(true, 1.4);
    EXPECT_FALSE(state.SaveDue(1.899));
    EXPECT_TRUE(state.SaveDue(1.9));
    EXPECT_DOUBLE_EQ(state.Display().effective.values.grass_density, 0.5);
    EXPECT_EQ(state.Display().effective.values.ui_blur, UiBlurQuality::Off);
    state.Saved(false);
    EXPECT_FALSE(state.SaveDue(100.0));
    EXPECT_EQ(state.Display().status, "Save failed; graphics are active for this session");
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Medium);
    state.ScheduleSave(2.0);
    EXPECT_TRUE(state.SaveDue(2.5));
    state.Saved(true);
    EXPECT_EQ(state.Display().status, "Graphics saved");
    EXPECT_FALSE(state.SavePending());
}

TEST(GraphicsSettingsState, ApplyFailureRetainsSavedSelectionAndNeverSchedulesItsSave) {
    GraphicsSettings settings;
    settings.overrides["render_scale"] = 0.75;
    GraphicsSettingsState state(settings, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Low)));
    state.Applied(false, 1.0);
    EXPECT_FALSE(state.Pending());
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Ultra);
    EXPECT_EQ(state.Settings().overrides, settings.overrides);
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.75);
    EXPECT_NE(state.Display().status.find("apply failed"), std::string::npos);
    EXPECT_FALSE(state.SaveDue(100.0));
}

TEST(GraphicsSettingsState, DetectApplyUsesFreshInputsClearsOverridesAndReportsHardware) {
    GraphicsSettings settings;
    settings.overrides["render_scale"] = 0.5;
    GraphicsSettingsState state(settings, {});
    GraphicsEdit detect;
    detect.kind = GraphicsEditKind::DetectApply;
    EXPECT_FALSE(state.Stage(detect));
    EXPECT_FALSE(state.Pending());
    EXPECT_EQ(state.Display().status, "Hardware detection unavailable");
    GraphicsHardwareInputs inputs;
    inputs.vendor = "NVIDIA";
    inputs.renderer = "NVIDIA GeForce RTX 5070 Ti";
    inputs.version = "4.6";
    inputs.logicalProcessorCount = 16;
    inputs.dedicatedVideoMemoryBytes = 8ULL * 1024 * 1024 * 1024;
    ASSERT_TRUE(state.Stage(detect, inputs));
    state.Applied(true, 0.0);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Ultra);
    EXPECT_TRUE(state.Settings().overrides.empty());
    EXPECT_EQ(state.Settings().detection.rules_revision, kGraphicsPresetRulesRevision);
    EXPECT_NE(state.Display().hardware.find("RTX 5070 Ti"), std::string::npos);
    EXPECT_NE(state.Display().hardware.find("CPU threads: 16"), std::string::npos);
    EXPECT_NE(state.Display().hardware.find("VRAM: 8192 MiB"), std::string::npos);
    EXPECT_NE(state.Display().hardware.find("Recommended: Ultra"), std::string::npos);
    inputs.logicalProcessorCount = 4;
    inputs.dedicatedVideoMemoryBytes.reset();
    ASSERT_TRUE(state.Stage(detect, inputs));
    state.Applied(true, 1.0);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Low);
    EXPECT_NE(state.Display().hardware.find("VRAM: unknown"), std::string::npos);
    const auto before = state.Settings().detection;
    ASSERT_TRUE(state.Stage(detect, inputs));
    state.Applied(false, 2.0);
    EXPECT_EQ(state.Settings().detection, before);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Low);
}

TEST(GraphicsSettingsState, SavingPriorSuccessfulEditsDoesNotHideALaterApplyFailure) {
    GraphicsSettingsState state({}, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::High)));
    state.Applied(true, 0.0);
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Low)));
    state.Applied(false, 0.1);
    EXPECT_TRUE(state.SaveDue(0.5));
    state.Saved(true);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::High);
    EXPECT_NE(state.Display().status.find("apply failed"), std::string::npos);
}

TEST(GraphicsSettingsState, SessionOverridesDisplayEffectiveValuesAndNeverEnterSavedSettings) {
    GraphicsSettings settings;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::High));
    const GraphicsOverrides session{{"render_scale", 0.67}, {"volumetric_quality", 0.0}};
    GraphicsSettingsState state(settings, session);
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.67);
    EXPECT_EQ(state.Display().effective.values.volumetric_quality, 0);
    EXPECT_EQ(state.Display().effective.session_overrides, session);
    EXPECT_NE(state.Display().preset_label.find("Session overrides active"), std::string::npos);
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Ultra)));
    state.Applied(true, 1.0);
    ASSERT_TRUE(state.Stage(GraphicsEdit{}));
    state.Applied(true, 2.0);
    EXPECT_TRUE(state.Settings().overrides.empty());
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.67);
    ResolvedGraphics saved;
    std::string error;
    ASSERT_TRUE(ResolveGraphics(state.Settings(), {}, saved, error));
    EXPECT_DOUBLE_EQ(saved.values.render_scale, 1.0);
    EXPECT_EQ(saved.values.volumetric_quality, 1);
}

TEST(GraphicsSettingsState, StartupMigrationReportDistinguishesFirstLaunchFromExistingChoices) {
    SystemConfig first;
    EXPECT_TRUE(first.settings_load_report().needs_first_launch_detection);
    SystemConfig legacy;
    legacy.OverlayUserFromJsonString(R"({"user":{"video":{"render_scale":0.8}}})");
    EXPECT_FALSE(legacy.settings_load_report().needs_first_launch_detection);
    EXPECT_EQ(BuildGraphicsSettingsDisplay(legacy.user().graphics, {}, "").preset_label,
              "Custom (base: Ultra)");
    SystemConfig recovery;
    recovery.OverlayUserFromJsonString("{");
    EXPECT_FALSE(recovery.settings_load_report().needs_first_launch_detection);
    const auto display = BuildGraphicsSettingsDisplay(
        recovery.user().graphics, {}, recovery.settings_load_report().recovery_reason);
    EXPECT_FALSE(display.status.empty());
    EXPECT_EQ(display.preset_label, "Ultra");
}
} // namespace

namespace {
using namespace Luminumbra::Client::UiTexture;

std::vector<unsigned char>
MakeTga(unsigned char depth, unsigned char descriptor, unsigned char id_length = 0) {
    std::vector<unsigned char> bytes(18 + static_cast<std::size_t>(id_length), 0);
    bytes[0] = id_length;
    bytes[2] = 2;
    bytes[12] = 2;
    bytes[14] = 2;
    bytes[16] = depth;
    bytes[17] = descriptor;
    std::fill(bytes.begin() + 18, bytes.end(), 0xa5);
    constexpr std::array<std::array<unsigned char, 4>, 4> bgra{
        {{30, 60, 90, 255}, {100, 150, 200, 128}, {7, 8, 9, 0}, {255, 128, 64, 64}}};
    for (std::size_t y = 0; y < 2; ++y) {
        const auto row = (descriptor & 0x20) != 0 ? y : 1 - y;
        for (std::size_t x = 0; x < 2; ++x) {
            const auto& pixel = bgra[row * 2 + x];
            bytes.insert(bytes.end(), pixel.begin(), pixel.begin() + depth / 8);
        }
    }
    return bytes;
}

TEST(UiTgaTexture, ExactPixelsForBothDepthsOriginsAndImageIds) {
    const std::vector<unsigned char> rgb_expected{
        90, 60, 30, 255, 200, 150, 100, 255, 9, 8, 7, 255, 64, 128, 255, 255};
    const std::vector<unsigned char> rgba_expected{
        90, 60, 30, 255, 100, 75, 50, 128, 0, 0, 0, 0, 16, 32, 64, 64};
    for (const auto depth : std::array<unsigned char, 2>{24, 32}) {
        for (const auto origin : std::array<unsigned char, 2>{0, 32}) {
            for (const auto id_length : std::array<unsigned char, 3>{0, 3, 255}) {
                auto bytes = MakeTga(depth, origin, id_length);
                // Screen-coordinate origins do not change the stored pixel rectangle.
                bytes[8] = 7;
                bytes[10] = 9;
                bytes.push_back(0xff); // A trailing footer is not part of the pixel payload.
                const auto image = DecodeTga(bytes);
                EXPECT_EQ(image.width, 2);
                EXPECT_EQ(image.height, 2);
                EXPECT_EQ(image.pixels, depth == 24 ? rgb_expected : rgba_expected);
            }
        }
    }
    EXPECT_EQ(DecodeTga(MakeTga(32, 0x28)).pixels, rgba_expected);
}

TEST(UiTgaTexture, RefusesEveryTruncatedHeaderAndPayloadPrefix) {
    for (const auto depth : std::array<unsigned char, 2>{24, 32}) {
        for (const auto origin : std::array<unsigned char, 2>{0, 32}) {
            for (const auto id_length : std::array<unsigned char, 3>{0, 3, 255}) {
                const auto bytes = MakeTga(depth, origin, id_length);
                for (std::size_t size = 0; size < bytes.size(); ++size) {
                    const auto image = DecodeTga(std::span(bytes).first(size));
                    EXPECT_TRUE(image.pixels.empty()) << "prefix size " << size;
                    EXPECT_EQ(image.width, 0);
                    EXPECT_EQ(image.height, 0);
                }
            }
        }
    }
}

TEST(UiTgaTexture, RefusesUnsupportedPixelWidthsAndHeaderFields) {
    for (unsigned int depth = 0; depth <= 255; ++depth) {
        if (depth == 24 || depth == 32) {
            continue;
        }
        auto bytes = MakeTga(32, 0x20);
        bytes[16] = static_cast<unsigned char>(depth);
        EXPECT_TRUE(DecodeTga(bytes).pixels.empty()) << "depth " << depth;
    }
    for (unsigned int type = 0; type <= 255; ++type) {
        if (type == 2) {
            continue;
        }
        auto bytes = MakeTga(24, 0x20);
        bytes[2] = static_cast<unsigned char>(type);
        EXPECT_TRUE(DecodeTga(bytes).pixels.empty()) << "image type " << type;
    }
    for (const auto field : std::array<std::size_t, 7>{1, 2, 3, 4, 5, 6, 7}) {
        auto bytes = MakeTga(24, 0x20);
        bytes[field] = 1;
        EXPECT_TRUE(DecodeTga(bytes).pixels.empty()) << "header field " << field;
    }
    for (const auto depth : std::array<unsigned char, 2>{24, 32}) {
        for (unsigned int descriptor = 0; descriptor <= 255; ++descriptor) {
            auto bytes = MakeTga(depth, 0x20);
            bytes[17] = static_cast<unsigned char>(descriptor);
            const bool supported = descriptor == 0 || descriptor == 32 ||
                                   (depth == 32 && (descriptor == 8 || descriptor == 40));
            EXPECT_EQ(!DecodeTga(bytes).pixels.empty(), supported) << "descriptor " << descriptor;
        }
    }
}

TEST(UiTgaTexture, ChecksZeroAndArithmeticBoundaryDimensionsBeforeAllocation) {
    for (const unsigned int width : {0u, 1u, 32767u, 32768u, 65535u}) {
        for (const unsigned int height : {0u, 1u, 32767u, 32768u, 65535u}) {
            std::vector<unsigned char> bytes(18, 0);
            bytes[2] = 2;
            bytes[12] = static_cast<unsigned char>(width & 0xff);
            bytes[13] = static_cast<unsigned char>(width >> 8);
            bytes[14] = static_cast<unsigned char>(height & 0xff);
            bytes[15] = static_cast<unsigned char>(height >> 8);
            bytes[16] = 32;
            EXPECT_TRUE(DecodeTga(bytes).pixels.empty());
        }
    }
    // The high bit of a dimension is unsigned; a complete thin image remains valid.
    for (const unsigned int width : {32768u, 65535u}) {
        std::vector<unsigned char> bytes(18 + static_cast<std::size_t>(width) * 3, 0);
        bytes[2] = 2;
        bytes[12] = static_cast<unsigned char>(width & 0xff);
        bytes[13] = static_cast<unsigned char>(width >> 8);
        bytes[14] = 1;
        bytes[16] = 24;
        const auto image = DecodeTga(bytes);
        EXPECT_EQ(image.width, static_cast<int>(width));
        EXPECT_EQ(image.height, 1);
        ASSERT_EQ(image.pixels.size(), static_cast<std::size_t>(width) * 4);
        EXPECT_EQ(image.pixels.back(), 255);
    }
}

TEST(UiTgaTexture, UploadSpanMustExactlyMatchPositiveRepresentableDimensions) {
    const std::array<unsigned char, 8> pixels{};
    EXPECT_TRUE(ValidRgbaData(pixels.data(), pixels.size(), 2, 1));
    EXPECT_TRUE(ValidRgbaData(pixels.data(), pixels.size(), 1, 2));
    EXPECT_FALSE(ValidRgbaData(nullptr, 0, 1, 1));
    EXPECT_FALSE(ValidRgbaData(nullptr, 4, 1, 1));
    EXPECT_FALSE(ValidRgbaData(pixels.data(), 7, 2, 1));
    EXPECT_FALSE(ValidRgbaData(pixels.data(), pixels.size(), 1, 1));
    for (const int invalid : {0, -1, std::numeric_limits<int>::min()}) {
        EXPECT_FALSE(ValidRgbaData(pixels.data(), pixels.size(), invalid, 1));
        EXPECT_FALSE(ValidRgbaData(pixels.data(), pixels.size(), 1, invalid));
    }
    constexpr int max_dimension = std::numeric_limits<int>::max();
    EXPECT_EQ(RgbaByteCount(max_dimension, max_dimension), 0);
    EXPECT_FALSE(ValidRgbaData(pixels.data(), pixels.size(), max_dimension, 2));
    EXPECT_FALSE(ValidRgbaData(pixels.data(), pixels.size(), 2, max_dimension));
    EXPECT_EQ(RgbaByteCount(2, 1), 8);
}

struct TgaFiles {
    std::vector<unsigned char> bytes = MakeTga(24, 0x20, 3);
    std::size_t position = 0;
    std::size_t short_read_call = 0;
    std::size_t throw_read_call = 0;
    std::size_t fail_seek_call = 0;
    std::size_t read_calls = 0;
    std::size_t seek_calls = 0;
    int close_calls = 0;
    bool fail_open = false;
    bool fail_tell = false;
    bool overstate_size = false;

    int Open(const char*) const {
        return fail_open ? 0 : 1;
    }
    void Close(int) {
        ++close_calls;
    }
    bool Seek(int, long, int origin) {
        ++seek_calls;
        if (seek_calls == fail_seek_call) {
            return false;
        }
        position = origin == SEEK_END ? bytes.size() : 0;
        return true;
    }
    std::size_t Tell(int) const {
        if (fail_tell) {
            return std::numeric_limits<std::size_t>::max();
        }
        return position + (overstate_size ? 100 : 0);
    }
    std::size_t Read(void* dest, std::size_t size, int) {
        ++read_calls;
        if (read_calls == throw_read_call) {
            throw std::runtime_error("read failure");
        }
        auto count = std::min(size, bytes.size() - position);
        if (read_calls == short_read_call && count > 0) {
            --count;
        }
        std::memcpy(dest, bytes.data() + position, count);
        position += count;
        return count;
    }
};

TEST(UiTgaTexture, FileFailuresRefuseAndCloseExactlyOnce) {
    TgaFiles unopened;
    unopened.fail_open = true;
    EXPECT_TRUE(LoadTga(unopened, "texture").pixels.empty());
    EXPECT_EQ(unopened.close_calls, 0);
    EXPECT_EQ(unopened.seek_calls, 0);
    for (const auto call : std::array<std::size_t, 2>{1, 2}) {
        TgaFiles seek_failure;
        seek_failure.fail_seek_call = call;
        EXPECT_TRUE(LoadTga(seek_failure, "texture").pixels.empty());
        EXPECT_EQ(seek_failure.close_calls, 1);
        EXPECT_EQ(seek_failure.read_calls, 0);

        TgaFiles short_read;
        short_read.short_read_call = call;
        EXPECT_TRUE(LoadTga(short_read, "texture").pixels.empty());
        EXPECT_EQ(short_read.close_calls, 1);
        EXPECT_EQ(short_read.read_calls, call);

        TgaFiles throwing_read;
        throwing_read.throw_read_call = call;
        EXPECT_THROW(LoadTga(throwing_read, "texture"), std::runtime_error);
        EXPECT_EQ(throwing_read.close_calls, 1);
    }
    TgaFiles tell_failure;
    tell_failure.fail_tell = true;
    EXPECT_TRUE(LoadTga(tell_failure, "texture").pixels.empty());
    EXPECT_EQ(tell_failure.close_calls, 1);
    EXPECT_EQ(tell_failure.read_calls, 0);

    for (std::size_t size = 0; size < 33; ++size) {
        TgaFiles partial;
        partial.bytes.resize(size);
        EXPECT_TRUE(LoadTga(partial, "texture").pixels.empty());
        EXPECT_EQ(partial.close_calls, 1);
    }
    TgaFiles changed_file;
    changed_file.bytes.pop_back();
    changed_file.overstate_size = true;
    EXPECT_TRUE(LoadTga(changed_file, "texture").pixels.empty());
    EXPECT_EQ(changed_file.close_calls, 1);
    EXPECT_EQ(changed_file.read_calls, 2);

    TgaFiles invalid;
    invalid.bytes[16] = 16;
    EXPECT_TRUE(LoadTga(invalid, "texture").pixels.empty());
    EXPECT_EQ(invalid.close_calls, 1);
    EXPECT_EQ(invalid.read_calls, 1);
}

TEST(UiTgaTexture, FileSuccessMatchesPureDecoderAndClosesExactlyOnce) {
    for (const auto depth : std::array<unsigned char, 2>{24, 32}) {
        for (const auto origin : std::array<unsigned char, 2>{0, 32}) {
            TgaFiles files;
            files.bytes = MakeTga(depth, origin, 255);
            files.bytes.push_back(0xee);
            const auto image = LoadTga(files, "texture");
            EXPECT_EQ(image.pixels, DecodeTga(files.bytes).pixels);
            EXPECT_EQ(image.width, 2);
            EXPECT_EQ(image.height, 2);
            EXPECT_EQ(files.close_calls, 1);
            EXPECT_EQ(files.read_calls, 2);
            EXPECT_EQ(files.position, files.bytes.size() - 1);
        }
    }
}
} // namespace
