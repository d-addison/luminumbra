#include "ui/UiLayout.h"

#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <locale>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace Luminumbra::Client::UI;

UiLayout Fixture() {
    return {.screen = "menu\"\n\u03a9",
            .width = 800,
            .height = 600,
            .rml_elements = {{.id = "root",
                              .tag = "body",
                              .text = "direct\\text",
                              .rect = {1.25, 2.5, 300, 200},
                              .clip = UiLayoutRect{0, 0, 800, 600},
                              .clip_masked = true,
                              .z = std::nullopt,
                              .depth = 0,
                              .parent = std::nullopt},
                             {.id = "child",
                              .tag = "div",
                              .text = "",
                              .rect = {3, 4, 5, 6},
                              .clip = std::nullopt,
                              .clip_masked = false,
                              .z = std::nullopt,
                              .depth = 1,
                              .parent = 0}},
            .imgui_windows = {
                {.title = "debug", .rect = {7, 8, 9, 10}, .collapsed = true, .visible = false}}};
}

TEST(UiLayoutSerializer, ExactSchemaAndBytesAreStableWithoutGl) {
    const auto expected =
        "{\"schema\":1,\"screen\":\"menu\\\"\\n\u03a9\",\"width\":800,\"height\":600,"
        "\"rml_elements\":[{\"id\":\"root\",\"tag\":\"body\",\"text\":\"direct\\\\text\","
        "\"rect\":{\"x\":1.250,\"y\":2.500,\"w\":300.000,\"h\":200.000},"
        "\"clip\":{\"x\":0.000,\"y\":0.000,\"w\":800.000,\"h\":600.000},"
        "\"clip_masked\":true,\"z\":null,\"depth\":0,\"parent\":null},"
        "{\"id\":\"child\",\"tag\":\"div\",\"text\":\"\","
        "\"rect\":{\"x\":3.000,\"y\":4.000,\"w\":5.000,\"h\":6.000},"
        "\"clip\":null,\"clip_masked\":false,\"z\":null,\"depth\":1,\"parent\":0}],"
        "\"imgui_windows\":[{\"title\":\"debug\","
        "\"rect\":{\"x\":7.000,\"y\":8.000,\"w\":9.000,\"h\":10.000},"
        "\"collapsed\":true,\"visible\":false}]}\n";
    const auto first = SerializeUiLayout(Fixture());
    EXPECT_EQ(first, expected);
    EXPECT_EQ(SerializeUiLayout(Fixture()), first);
    const auto parsed = nlohmann::json::parse(first);
    EXPECT_EQ(parsed.at("schema"), 1);
    EXPECT_EQ(parsed.at("rml_elements").at(1).at("parent"), 0);
}

class CommaPunctuation final : public std::numpunct<char> {
protected:
    char do_decimal_point() const override {
        return ',';
    }
};

class ScopedCommaLocale {
public:
    ScopedCommaLocale()
        : m_previous(std::locale()) {
        std::locale::global(std::locale(m_previous, new CommaPunctuation));
    }
    ~ScopedCommaLocale() {
        std::locale::global(m_previous);
    }
    ScopedCommaLocale(const ScopedCommaLocale&) = delete;
    ScopedCommaLocale& operator=(const ScopedCommaLocale&) = delete;

private:
    std::locale m_previous;
};

TEST(UiLayoutSerializer, FixedPrecisionIgnoresGlobalLocale) {
    const auto expected = SerializeUiLayout(Fixture());
    const ScopedCommaLocale comma_locale;
    EXPECT_EQ(SerializeUiLayout(Fixture()), expected);
}

TEST(UiLayoutSerializer, EmptyArraysAndKnownZRemainTyped) {
    const UiLayout empty{
        .screen = "empty", .width = 1, .height = 1, .rml_elements = {}, .imgui_windows = {}};
    EXPECT_EQ(SerializeUiLayout(empty),
              "{\"schema\":1,\"screen\":\"empty\",\"width\":1,\"height\":1,"
              "\"rml_elements\":[],\"imgui_windows\":[]}\n");
    auto layout = Fixture();
    layout.rml_elements[1].z = 0;
    EXPECT_EQ(nlohmann::json::parse(SerializeUiLayout(layout))["rml_elements"][1]["z"], 0);
}

TEST(UiLayoutSerializer, InvalidDimensionsRefuse) {
    for (const int dimension : {0, -1}) {
        auto layout = Fixture();
        layout.width = dimension;
        EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
        layout = Fixture();
        layout.height = dimension;
        EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
    }
}

TEST(UiLayoutSerializer, NonFiniteGeometryRefuses) {
    for (const double number : {std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
        auto layout = Fixture();
        layout.rml_elements[0].rect.x = number;
        EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
        layout = Fixture();
        layout.rml_elements[0].clip = UiLayoutRect{0, number, 800, 600};
        EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
        layout = Fixture();
        layout.imgui_windows[0].rect.w = number;
        EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
    }
}

TEST(UiLayoutSerializer, NegativeSizesAndInvalidParentLinksRefuse) {
    auto layout = Fixture();
    layout.rml_elements[0].rect.h = -1;
    EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
    layout = Fixture();
    layout.rml_elements[1].parent = 1;
    EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
    layout.rml_elements[1].parent = std::numeric_limits<std::size_t>::max();
    EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
    layout.rml_elements[1].parent = 0;
    layout.rml_elements[1].depth = 0;
    EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
    layout.rml_elements[1].parent.reset();
    layout.rml_elements[1].depth = 1;
    EXPECT_THROW(SerializeUiLayout(layout), std::invalid_argument);
}

TEST(UiLayoutSerializer, ItemDepthStringLimitsAndInvalidUtf8Refuse) {
    auto layout = Fixture();
    layout.rml_elements.resize(kUiLayoutMaxElements + 1);
    EXPECT_THROW(SerializeUiLayout(layout), std::length_error);
    layout = Fixture();
    layout.imgui_windows.resize(kUiLayoutMaxWindows + 1);
    EXPECT_THROW(SerializeUiLayout(layout), std::length_error);
    layout = Fixture();
    layout.rml_elements[1].depth = kUiLayoutMaxDepth + 1;
    EXPECT_THROW(SerializeUiLayout(layout), std::length_error);
    layout = Fixture();
    layout.screen.assign(kUiLayoutMaxStringBytes + 1, 'x');
    EXPECT_THROW(SerializeUiLayout(layout), std::length_error);
    layout = Fixture();
    layout.screen.assign(kUiLayoutMaxStringBytes, 'x');
    EXPECT_THROW(SerializeUiLayout(layout), std::length_error) << "aggregate string bound";
    layout = Fixture();
    layout.screen = std::string(1, '\xff');
    EXPECT_THROW(SerializeUiLayout(layout), nlohmann::json::type_error);
}

} // namespace
