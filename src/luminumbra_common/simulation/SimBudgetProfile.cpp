#include "simulation/SimBudgetProfile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace luminumbra::simulation {
namespace {
using Json = nlohmann::json;
using Luminumbra::world::RegionSchedulerConfig;

constexpr std::size_t kMaxDepth = 8;
constexpr std::size_t kMaxShownKey = 64;
constexpr std::string_view kSchemaV1 = "luminumbra.sim_budget_profile.v1";
constexpr std::string_view kSchemaPrefix = "luminumbra.sim_budget_profile.v";
constexpr std::uint64_t kMaxHoldTicks = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kMaxReducedShift = 16;
constexpr std::size_t kMaxNameLength = 64;

std::string Quoted(std::string_view text) {
    return "\"" + std::string(text.substr(0, kMaxShownKey)) + "\"";
}

void Reject(ProfileParse& out, ProfileError error, std::string detail) {
    out = ProfileParse{};
    out.error = error;
    out.detail = std::move(detail);
}

bool IsName(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

// First pass: rejects duplicate keys, floats, negative integers, arrays and deep nesting.
// The first failure is recorded; later callbacks never overwrite it.
class ProfileSax : public Json::json_sax_t {
public:
    ProfileError error() const {
        return error_;
    }
    const std::string& detail() const {
        return detail_;
    }

    bool null() override {
        return true;
    }
    bool boolean(bool) override {
        return true;
    }
    bool number_integer(std::int64_t value) override {
        if (value < 0)
            return Fail(ProfileError::OutOfRange, "negative integer");
        return true;
    }
    bool number_unsigned(std::uint64_t) override {
        return true;
    }
    bool number_float(double, const std::string&) override {
        return Fail(ProfileError::Malformed, "floats are not allowed");
    }
    bool string(std::string&) override {
        return true;
    }
    bool binary(Json::binary_t&) override {
        return Fail(ProfileError::Malformed, "binary values are not allowed");
    }
    bool start_object(std::size_t) override {
        if (open_.size() >= kMaxDepth)
            return Fail(ProfileError::Malformed, "nesting is deeper than 8 levels");
        open_.emplace_back();
        return true;
    }
    bool key(std::string& name) override {
        if (open_.empty())
            return Fail(ProfileError::Malformed, "key outside an object");
        if (!open_.back().insert(name).second)
            return Fail(ProfileError::Duplicate, "duplicate key " + Quoted(name));
        return true;
    }
    bool end_object() override {
        if (open_.empty())
            return Fail(ProfileError::Malformed, "unbalanced object");
        open_.pop_back();
        return true;
    }
    bool start_array(std::size_t) override {
        return Fail(ProfileError::Malformed, "arrays are not allowed");
    }
    bool end_array() override {
        return Fail(ProfileError::Malformed, "arrays are not allowed");
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return Fail(ProfileError::Malformed, "invalid JSON");
    }

private:
    bool Fail(ProfileError error, std::string detail) {
        if (error_ == ProfileError::None) {
            error_ = error;
            detail_ = std::move(detail);
        }
        return false;
    }

    std::vector<std::set<std::string>> open_;
    ProfileError error_ = ProfileError::None;
    std::string detail_;
};

// Reads a section of name -> unsigned integer members (stages or residency).
bool ParseNamedCaps(const Json& node,
                    std::string_view section,
                    std::size_t max_entries,
                    std::map<std::string, std::uint64_t>& out,
                    ProfileParse& result) {
    if (!node.is_object()) {
        Reject(result, ProfileError::Malformed, std::string(section) + " must be an object");
        return false;
    }
    for (const auto& item : node.items()) {
        const std::string& name = item.key();
        if (!item.value().is_number_unsigned()) {
            Reject(result,
                   ProfileError::Malformed,
                   Quoted(name) + " in " + std::string(section) + " must be an unsigned integer");
            return false;
        }
        if (!IsName(name)) {
            Reject(result,
                   ProfileError::OutOfRange,
                   "invalid name " + Quoted(name) + " in " + std::string(section));
            return false;
        }
        out.emplace(name, item.value().get<std::uint64_t>());
    }
    if (out.size() > max_entries) {
        Reject(result,
               ProfileError::TooLarge,
               std::string(section) + " has more than " + std::to_string(max_entries) + " entries");
        return false;
    }
    return true;
}

// Reads one optional region_scheduler member. Returns false (with result set) on failure.
bool ReadSchedulerMember(const Json& node,
                         const char* key,
                         std::uint64_t max_value,
                         std::uint64_t min_value,
                         std::uint64_t& out,
                         ProfileParse& result) {
    const auto member = node.find(key);
    if (member == node.end())
        return true;
    const std::string name = std::string("region_scheduler.") + key;
    if (!member->is_number_unsigned()) {
        Reject(result, ProfileError::Malformed, name + " must be an unsigned integer");
        return false;
    }
    const auto value = member->get<std::uint64_t>();
    if (value < min_value || value > max_value) {
        Reject(result,
               ProfileError::OutOfRange,
               name + " is outside " + std::to_string(min_value) + ".." +
                   std::to_string(max_value));
        return false;
    }
    out = value;
    return true;
}

ProfileParse ParseBody(const std::string& text) {
    ProfileParse result;
    if (text.find('\0') != std::string::npos) {
        Reject(result, ProfileError::Malformed, "raw NUL in profile");
        return result;
    }
    ProfileSax sax;
    if (!Json::sax_parse(text, &sax)) {
        if (sax.error() == ProfileError::None)
            Reject(result, ProfileError::Malformed, "invalid JSON");
        else
            Reject(result, sax.error(), sax.detail());
        return result;
    }

    const Json doc = Json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        Reject(result, ProfileError::Malformed, "invalid JSON");
        return result;
    }
    if (!doc.is_object()) {
        Reject(result, ProfileError::Malformed, "top level must be an object");
        return result;
    }
    const auto schema = doc.find("schema");
    if (schema == doc.end()) {
        Reject(result, ProfileError::Malformed, "missing schema");
        return result;
    }
    if (!schema->is_string()) {
        Reject(result, ProfileError::Malformed, "schema must be a string");
        return result;
    }
    const std::string_view version = schema->get_ref<const std::string&>();
    if (version != kSchemaV1) {
        if (version.starts_with(kSchemaPrefix))
            Reject(result, ProfileError::FutureVersion, "unsupported schema " + Quoted(version));
        else
            Reject(result, ProfileError::Malformed, "unknown schema " + Quoted(version));
        return result;
    }

    for (const auto& item : doc.items()) {
        const std::string& key = item.key();
        if (key != "schema" && key != "region_scheduler" && key != "stages" && key != "residency") {
            Reject(result, ProfileError::UnknownKey, "unknown key " + Quoted(key));
            return result;
        }
    }

    if (const auto node = doc.find("region_scheduler"); node != doc.end()) {
        if (!node->is_object()) {
            Reject(result, ProfileError::Malformed, "region_scheduler must be an object");
            return result;
        }
        for (const auto& item : node->items()) {
            const std::string& key = item.key();
            if (key != "work_limit" && key != "hold_ticks" && key != "reduced_shift") {
                Reject(result,
                       ProfileError::UnknownKey,
                       "unknown region_scheduler key " + Quoted(key));
                return result;
            }
        }
        RegionSchedulerConfig config;
        if (!ReadSchedulerMember(*node,
                                 "work_limit",
                                 std::numeric_limits<std::uint64_t>::max(),
                                 0,
                                 config.work_limit,
                                 result))
            return result;
        std::uint64_t hold_ticks = config.hold_ticks;
        if (!ReadSchedulerMember(*node, "hold_ticks", kMaxHoldTicks, 1, hold_ticks, result))
            return result;
        config.hold_ticks = static_cast<std::uint32_t>(hold_ticks);
        std::uint64_t reduced_shift = config.reduced_shift;
        if (!ReadSchedulerMember(
                *node, "reduced_shift", kMaxReducedShift, 1, reduced_shift, result))
            return result;
        config.reduced_shift = static_cast<std::uint8_t>(reduced_shift);
        result.profile.region_scheduler = config;
    }

    if (const auto node = doc.find("stages"); node != doc.end()) {
        if (!ParseNamedCaps(
                *node, "stages", kMaxProfileStages, result.profile.stage_max_work, result))
            return result;
    }
    if (const auto node = doc.find("residency"); node != doc.end()) {
        if (!ParseNamedCaps(*node,
                            "residency",
                            kMaxProfileResidencyClasses,
                            result.profile.residency_max_bytes,
                            result))
            return result;
    }
    return result;
}

} // namespace

std::string_view ProfileErrorName(ProfileError error) {
    switch (error) {
        case ProfileError::None:
            return "none";
        case ProfileError::Malformed:
            return "malformed";
        case ProfileError::UnknownKey:
            return "unknown_key";
        case ProfileError::OutOfRange:
            return "out_of_range";
        case ProfileError::Duplicate:
            return "duplicate";
        case ProfileError::TooLarge:
            return "too_large";
        case ProfileError::FutureVersion:
            return "future_version";
    }
    return "malformed";
}

ProfileParse ParseSimBudgetProfile(std::string_view json) noexcept {
    ProfileParse result;
    try {
        if (json.size() > kMaxProfileBytes) {
            Reject(result, ProfileError::TooLarge, "profile is larger than 64 KiB");
            return result;
        }
        return ParseBody(std::string(json));
    } catch (...) {
        // Building the detail string may itself fail; fall back to a value-only error.
        try {
            Reject(result, ProfileError::Malformed, "profile could not be parsed");
        } catch (...) {
            result = ProfileParse{};
            result.error = ProfileError::Malformed;
        }
        return result;
    }
}

RegionSchedulerConfig ToRegionSchedulerConfig(const SimBudgetProfile& profile) {
    if (profile.region_scheduler)
        return *profile.region_scheduler;
    return RegionSchedulerConfig{};
}

BudgetVerdict EvaluateSimBudget(const SimBudgetProfile& profile,
                                std::span<const StageTick> ticks,
                                std::span<const ResidencySample> samples) {
    BudgetVerdict verdict;
    std::set<std::string> observed_stages;
    std::set<std::string> observed_classes;

    for (const auto& tick : ticks) {
        observed_stages.insert(tick.stage);
        const auto cap = profile.stage_max_work.find(tick.stage);
        if (cap != profile.stage_max_work.end() && tick.work > cap->second)
            verdict.violations.push_back(
                {BudgetViolation::Kind::Stage, tick.stage, tick.tick, tick.work, cap->second});
    }
    for (const auto& sample : samples) {
        observed_classes.insert(sample.cls);
        const auto cap = profile.residency_max_bytes.find(sample.cls);
        if (cap != profile.residency_max_bytes.end() && sample.peak_bytes > cap->second)
            verdict.violations.push_back(
                {BudgetViolation::Kind::Residency, sample.cls, 0, sample.peak_bytes, cap->second});
    }
    std::sort(verdict.violations.begin(),
              verdict.violations.end(),
              [](const BudgetViolation& a, const BudgetViolation& b) {
                  return std::tie(a.kind, a.name, a.tick, a.observed, a.limit) <
                         std::tie(b.kind, b.name, b.tick, b.observed, b.limit);
              });

    for (const auto& entry : profile.stage_max_work) {
        if (!observed_stages.contains(entry.first))
            verdict.unevaluated.push_back("stage:" + entry.first);
    }
    for (const auto& entry : profile.residency_max_bytes) {
        if (!observed_classes.contains(entry.first))
            verdict.unevaluated.push_back("residency:" + entry.first);
    }
    std::sort(verdict.unevaluated.begin(), verdict.unevaluated.end());

    verdict.no_violation = verdict.violations.empty();
    verdict.coverage_complete = verdict.unevaluated.empty();
    return verdict;
}

} // namespace luminumbra::simulation
