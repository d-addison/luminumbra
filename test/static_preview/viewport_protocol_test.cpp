#include "ViewportProtocol.h"
#include "authoring/PrefabDigest.h"
#include <gtest/gtest.h>
#include <limits>
using namespace Luminumbra::Viewport;
namespace {
Json State() {
    const Json matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return {{"kind", "state"},
            {"session", std::string(32, 'a')},
            {"sequence", 1},
            {"state",
             {{"generation_id", std::string(32, 'b')},
              {"manifest_sha256", std::string(64, 'c')},
              {"scene_revision", 0},
              {"camera_revision", 0},
              {"width", 1},
              {"height", 1},
              {"near_plane", .1},
              {"far_plane", 1000},
              {"view", matrix},
              {"projection", matrix},
              {"locals", Json::array()}}}};
}
} // namespace
TEST(ViewportProtocol, HmacMatchesIndependentSha256Vector) {
    Key key{};
    for (unsigned i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(i);
    const std::string text = "viewport protocol authentication";
    // Python hmac.digest(bytes(range(32)), b"viewport protocol authentication", "sha256").
    EXPECT_EQ(Mac({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, key),
              DecodeKey("08620161969a5e4d4193ab7c5ec2b6cb34133074113414388f552e5059c88231"));
}
TEST(ViewportProtocol, RoundtripAndEveryTruncationAreBounded) {
    auto header = State();
    Key key{};
    const auto bytes = Encode({header, {}}, key);
    EXPECT_EQ(Decode(bytes, key).header, header);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        EXPECT_THROW(Decode(std::span(bytes).first(i), key), std::exception);
    auto changed = bytes;
    changed[50] ^= 1;
    EXPECT_THROW(Decode(changed, key), std::exception);
    changed = bytes;
    changed[8] = 1;
    EXPECT_THROW(RecordSize(std::span(changed).first(16), true), std::exception);
}
TEST(ViewportProtocol, RefusesWrongTypesUnknownFieldsAndNonfiniteMatrices) {
    auto header = State();
    header["state"]["width"] = true;
    EXPECT_THROW(Encode({header, {}}, {}), std::exception);
    header = State();
    header["state"]["unexpected"] = 1;
    EXPECT_THROW(Encode({header, {}}, {}), std::exception);
    header = State();
    header["state"]["view"][0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(Encode({header, {}}, {}), std::exception);
    header = State();
    header["state"]["locals"] = {
        {{"instance_id", "1.bad"}, {"node_id", "node"}, {"matrix", header["state"]["view"]}}};
    EXPECT_THROW(Encode({header, {}}, {}), std::exception);
}
TEST(ViewportProtocol, RequiresExactFrameDepthCoverageAndAlphaJoin) {
    auto header = State();
    header["kind"] = "frame";
    header["planes_sha256"] = "d5fd8c2e2b4e947c31edc9d1c3fbe2d6c9f9e3bb513a3c8e8c764e4657659048";
    EXPECT_THROW(Encode({header, std::vector<std::uint8_t>(9)}, {}), std::exception);
    header["planes_sha256"] = "3e7077fd2f66d689e0cee6a7cf5b37bf2dca7c979af356d0a31cbc5c85605c7d";
    // Nine zero bytes form valid clear color/depth/coverage.
    EXPECT_NO_THROW(Encode({header, std::vector<std::uint8_t>(9)}, {}));
}
TEST(ViewportProtocol, AdmissionIsMonotonicAndGenerationManifestImmutable) {
    StateGate gate;
    auto header = State();
    gate.Accept(header);
    EXPECT_THROW(gate.Accept(header), std::exception);
    header["sequence"] = 2;
    header["state"]["view"][12] = 30;
    EXPECT_THROW(gate.Accept(header), std::exception);
    header["state"]["camera_revision"] = 1;
    gate.Accept(header);
    header["sequence"] = 3;
    header["state"]["generation_id"] = std::string(32, 'd');
    EXPECT_THROW(gate.Accept(header), std::exception);
    header["state"]["scene_revision"] = 1;
    gate.Accept(header);
    header["sequence"] = 4;
    header["state"]["generation_id"] = std::string(32, 'b');
    header["state"]["manifest_sha256"] = std::string(64, 'e');
    header["state"]["scene_revision"] = 2;
    EXPECT_THROW(gate.Accept(header), std::exception);
    Json stop = {{"kind", "stop"}, {"session", header["session"]}, {"sequence", 4}};
    gate.Accept(stop);
    stop["sequence"] = 5;
    EXPECT_THROW(gate.Accept(stop), std::exception);
}

TEST(ViewportProtocol, GenerationSwitchKeepsSessionRevisionsAndCameraChangeRules) {
    auto first = State();
    first["state"]["scene_revision"] = 1;
    first["state"]["camera_revision"] = 10;
    StateGate gate;
    gate.Accept(first);
    auto next = first;
    next["sequence"] = 2;
    next["state"]["generation_id"] = std::string(32, 'd');
    next["state"]["scene_revision"] = 2;
    next["state"]["camera_revision"] = 0;
    EXPECT_THROW(gate.Accept(next), std::exception);
    EXPECT_EQ(gate.last(), first);
    next["state"]["camera_revision"] = 10;
    next["state"]["view"][12] = 1;
    EXPECT_THROW(gate.Accept(next), std::exception);
    EXPECT_EQ(gate.last(), first);
    next["state"]["camera_revision"] = 11;
    EXPECT_NO_THROW(gate.Accept(next));
}

TEST(ViewportProtocol, CoalescedGenerationRoundtripPreservesAdmission) {
    auto first = State();
    first["state"]["scene_revision"] = 1;
    first["state"]["camera_revision"] = 10;
    auto middle = first;
    middle["sequence"] = 2;
    middle["state"]["generation_id"] = std::string(32, 'd');
    middle["state"]["scene_revision"] = 2;
    middle["state"]["camera_revision"] = 11;
    middle["state"]["view"][12] = 1;
    auto last = first;
    last["sequence"] = 3;
    last["state"]["scene_revision"] = 3;
    last["state"]["camera_revision"] = 12;
    last["state"]["view"][12] = 2;
    StateGate complete;
    complete.Accept(first);
    complete.Accept(middle);
    complete.Accept(last);
    StateGate coalesced;
    coalesced.Accept(first);
    EXPECT_NO_THROW(coalesced.Accept(last));
    EXPECT_EQ(complete.last(), coalesced.last());
}

TEST(ViewportProtocol, GenerationBoundAllowsRevisitsWithoutForgettingPins) {
    const auto generation = [](unsigned index) {
        constexpr char digits[] = "0123456789abcdef";
        return std::string(30, '0') + digits[index / 16] + digits[index % 16];
    };
    StateGate gate;
    auto header = State();
    for (unsigned i = 0; i < 64; ++i) {
        header["sequence"] = i + 1;
        header["state"]["scene_revision"] = i;
        header["state"]["generation_id"] = generation(i);
        gate.Accept(header);
    }
    header["sequence"] = 65;
    header["state"]["scene_revision"] = 64;
    header["state"]["generation_id"] = std::string(32, 'f');
    EXPECT_THROW(gate.Accept(header), std::exception);
    EXPECT_EQ(gate.last().at("sequence"), 64);
    header["state"]["generation_id"] = generation(0);
    EXPECT_NO_THROW(gate.Accept(header));
    header["sequence"] = 66;
    header["state"]["scene_revision"] = 65;
    header["state"]["manifest_sha256"] = std::string(64, 'e');
    EXPECT_THROW(gate.Accept(header), std::exception);
    EXPECT_EQ(gate.last().at("sequence"), 65);
}

TEST(ViewportProtocol, AuthenticatedInvalidJsonCannotBypassParsingRules) {
    Key key{};
    for (const std::string raw : {"{\"kind\":\"state\",\"kind\":\"stop\"}", "{\"kind\":NaN}"}) {
        std::vector<std::uint8_t> bytes{'L', 'V', 'P', '1'};
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(static_cast<std::uint8_t>(raw.size() >> (8 * i)));
        bytes.resize(16, 0);
        bytes.insert(bytes.end(), raw.begin(), raw.end());
        const auto mac = Mac(bytes, key);
        bytes.insert(bytes.begin() + 16, mac.begin(), mac.end());
        EXPECT_THROW(Decode(bytes, key), std::exception);
    }
}
TEST(ViewportProtocol, ValidPlaneHashesDoNotHideNonfiniteDepthOrWrongCoverage) {
    auto header = State();
    header["kind"] = "frame";
    for (const std::vector<std::uint8_t>& payload :
         {std::vector<std::uint8_t>{1, 2, 3, 255, 0, 0, 0, 63, 0},
          std::vector<std::uint8_t>{1, 2, 3, 0, 0, 0, 0, 63, 1},
          std::vector<std::uint8_t>{1, 2, 3, 255, 0, 0, 192, 127, 1}}) {
        header["planes_sha256"] = Luminumbra::Authoring::Detail::Sha256(payload);
        EXPECT_THROW(Encode({header, payload}, {}), std::exception);
    }
}
