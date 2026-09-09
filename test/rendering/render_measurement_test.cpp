#include "luminumbra_client/app/RenderMeasurement.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::Client::Measurement;

void require(bool condition) {
    if (!condition)
        throw std::runtime_error("measurement assertion failed");
}

template<class Function>
void refuses(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("invalid measurement input was accepted");
}

std::string fixture(const char* name) {
    std::ifstream input(std::string(LUMINUMBRA_SOURCE_ROOT) + "/tools/perf/fixtures/" + name,
                        std::ios::binary);
    require(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct FakeQueryState {
    std::uint32_t nextId = 0;
    std::uint64_t clock = 0;
    bool supported = true;
    bool ready = false;
    int reads = 0;
    std::map<std::uint32_t, std::uint64_t> timestamps;
};
struct FakeQueries {
    std::shared_ptr<FakeQueryState> state;
    bool supported() const {
        return state->supported;
    }
    void create(std::uint32_t* ids, std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i)
            ids[i] = ++state->nextId;
    }
    void destroy(const std::uint32_t*, std::size_t) const {}
    void stamp(std::uint32_t id) const {
        state->timestamps[id] = (state->clock += 1000000);
    }
    bool available(std::uint32_t) const {
        return state->ready;
    }
    std::uint64_t result(std::uint32_t id) const {
        ++state->reads;
        return state->timestamps.at(id);
    }
};

void gpu_queries() {
    auto state = std::make_shared<FakeQueryState>();
    SampleRing samples(10);
    GpuQueryRing<FakeQueries> queries(samples, FakeQueries{state});
    queries.initialize();
    for (std::uint64_t frame = 400; frame < 410; ++frame) {
        samples.append(frame);
        queries.begin_frame(frame);
        queries.begin_pass(1);
        queries.end_pass(1);
        queries.end_frame();
    }
    // Eight late frames fill the ring. The ninth must drain the oldest before reuse.
    require(state->reads == 8);
    require(samples.at(400).gpu == 3.0 && samples.at(401).passSum == 1.0);
    require(!samples.at(402).gpu);
    queries.drain();
    require(state->reads == 40);
    for (std::uint64_t frame = 400; frame < 410; ++frame) {
        require(samples.at(frame).frame == frame);
        require(samples.at(frame).gpu == 3.0 && samples.at(frame).passSum == 1.0);
        require(!samples.at(frame).issued[0] && !samples.at(frame).passes[0]);
    }
    queries.shutdown();
    require(state->reads == 40); // already drained; never read a frame twice

    SampleRing incomplete(1);
    GpuQueryRing<FakeQueries> missingEnd(incomplete, FakeQueries{state});
    missingEnd.initialize();
    incomplete.append(0);
    missingEnd.begin_frame(0);
    missingEnd.begin_pass(1);
    missingEnd.end_frame();
    missingEnd.drain();
    require(incomplete.at(0).gpu.has_value());
    require(!incomplete.at(0).passSum && !incomplete.at(0).passes[1]);
    missingEnd.shutdown();

    SampleRing repeated(1);
    GpuQueryRing<FakeQueries> duplicate(repeated, FakeQueries{state});
    duplicate.initialize();
    repeated.append(0);
    duplicate.begin_frame(0);
    duplicate.begin_pass(1);
    duplicate.end_pass(1);
    duplicate.begin_pass(1);
    duplicate.end_pass(1);
    duplicate.end_frame();
    duplicate.drain();
    require(repeated.at(0).gpu.has_value() && !repeated.at(0).passSum);
    duplicate.shutdown();

    state->supported = false;
    SampleRing unsupported(1);
    GpuQueryRing<FakeQueries> unavailable(unsupported, FakeQueries{state});
    unavailable.initialize();
    unsupported.append(0);
    unavailable.begin_frame(0);
    unavailable.begin_pass(1);
    unavailable.end_pass(1);
    unavailable.end_frame();
    unavailable.drain();
    require(!unsupported.at(0).gpu && !unsupported.at(0).passSum);
    unavailable.shutdown();
}

void statistics() {
    std::istringstream input(fixture("distribution.txt"));
    std::size_t count = 0;
    input >> count;
    std::vector<double> values(count);
    for (double& value : values)
        input >> value;
    const auto actual = summarize(values);
    for (const double value : actual) {
        double expected = 0.0;
        input >> expected;
        require(std::abs(value - expected) < 1e-12);
    }
    require(summarize({7}) == (std::array<double, 5>{7, 7, 7, 7, 0}));
    require(percentile({0, 10, 20}, 0.95) == 19.0);
    refuses([] { summarize({}); });
    refuses([] { summarize({-1}); });
}

void traversal_deterministic() {
    std::istringstream first(fixture("surface-flight.traversal"));
    std::istringstream second(fixture("surface-flight.traversal"));
    const auto a = Traversal::read(first);
    const auto b = Traversal::read(second);
    require(a.expectedTicks == 1800 && a.duration == 60 && a.tickRate == 30);
    for (int tick = 0; tick <= a.expectedTicks; ++tick)
        require(a.sample(tick) == b.sample(tick));
    require(a.sample(0).position == (std::array<double, 3>{8, 80, 8}));
    require(a.sample(900).position == (std::array<double, 3>{248, 80, 8}));
    require(a.sample(1800).position == (std::array<double, 3>{488, 80, 8}));
    refuses([&] { a.sample(-1); });
    refuses([&] { a.sample(1801); });
}

void traversal_refusal() {
    const auto source = fixture("surface-flight.traversal");
    for (const auto& replacement : std::vector<std::pair<std::string, std::string>>{
             {"traversal.v1", "traversal.v2"},
             {"tick_rate 30", "tick_rate 60"},
             {"expected_ticks 1800", "expected_ticks 1799"},
             {"seed 424242", "seed -1"},
             {"preset_revision 6", "preset_revision 7"},
             {"speed_mps 8", "speed_mps 80"},
             {"cold_cache true", "cold_cache false"},
             {"point 248 80 8", "point 8 80 8"}}) {
        auto text = source;
        text.replace(text.find(replacement.first), replacement.first.size(), replacement.second);
        refuses([&] {
            std::istringstream input(text);
            Traversal::read(input);
        });
    }
    refuses([&] {
        std::istringstream input(source + "unknown 1");
        Traversal::read(input);
    });
    refuses([] {
        std::istringstream input;
        Traversal::read(input);
    });
}

void frame_ring() {
    SampleRing ring(3);
    ring.append(400);
    ring.append(401);
    ring.append(402);
    // Deliberately resolve in reverse order to prove attribution is by issue id.
    ring.at(402).gpu = 12.0;
    ring.at(400).gpu = 10.0;
    require(!ring.at(401).gpu.has_value());
    require(ring.at(400).gpu == 10.0 && ring.at(402).gpu == 12.0);
    refuses([&] { ring.append(403); });
    refuses([&] { ring.at(399); });
    refuses([&] { ring.at(403); });
    refuses([] { const SampleRing invalid(0); });
    refuses([] { const SampleRing invalid(kMaxFrames + 1); });
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2);
        const std::string name = argv[1];
        if (name == "queries")
            gpu_queries();
        else if (name == "statistics")
            statistics();
        else if (name == "traversal")
            traversal_deterministic();
        else if (name == "refusal")
            traversal_refusal();
        else if (name == "ring")
            frame_ring();
        else
            throw std::runtime_error("unknown test");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
