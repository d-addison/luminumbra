#include "RegionPanel.h"

#include <algorithm>
#include <cstdint>
#include <imgui.h>

namespace Luminumbra::Client::App {
namespace {
const char* StateName(world::RegionState region_state) {
    switch (region_state) {
        case world::RegionState::Active:
            return "Active";
        case world::RegionState::Reduced:
            return "Reduced";
        case world::RegionState::Frozen:
            return "Frozen";
    }
    return "Unknown";
}
unsigned long long DisplayInteger(std::uint64_t value) {
    return static_cast<unsigned long long>(value);
}
void DrawSummary(const world::RegionPanelSnapshot& snapshot) {
    ImGui::Text("Tick %llu; schedule %llu",
                DisplayInteger(snapshot.tick),
                DisplayInteger(snapshot.schedule_tick));
    ImGui::Text("Regions: Active %llu  Reduced %llu  Frozen %llu",
                DisplayInteger(snapshot.active),
                DisplayInteger(snapshot.reduced),
                DisplayInteger(snapshot.frozen));
    ImGui::Text("Creatures: Full %llu  Coarse %llu  Frozen %llu",
                DisplayInteger(snapshot.full_creatures),
                DisplayInteger(snapshot.coarse_creatures),
                DisplayInteger(snapshot.frozen_creatures));
    ImGui::Text("Work %llu / limit %llu; pressure %llu; due %llu",
                DisplayInteger(snapshot.work),
                DisplayInteger(snapshot.work_limit),
                DisplayInteger(snapshot.pressure),
                DisplayInteger(snapshot.due));
    ImGui::Text("Rejected anchors/work/capacity: %u / %u / %u",
                snapshot.rejected.anchors,
                snapshot.rejected.work,
                snapshot.rejected.capacity);
    ImGui::Text("Parked %zu bytes / %zu chunks; oldest tick %lld",
                snapshot.parked_bytes,
                snapshot.parked_count,
                static_cast<long long>(snapshot.oldest_parked_tick));
    ImGui::Text("Save tick %lld; %.3f ms; failures %u; requested %s",
                static_cast<long long>(snapshot.last_save_tick),
                snapshot.last_save_ms,
                snapshot.save_failures,
                snapshot.save_requested ? "yes" : "no");
    ImGui::Text("Water reserved/used/unused/unallocated: %llu / %llu / %llu / %llu; cells %llu",
                DisplayInteger(snapshot.water_reserved),
                DisplayInteger(snapshot.water_used),
                DisplayInteger(snapshot.water_unused),
                DisplayInteger(snapshot.water_unallocated),
                DisplayInteger(snapshot.water_cells));
    if (snapshot.water_bound_ticks)
        ImGui::Text("Water service bound: %llu ticks", DisplayInteger(*snapshot.water_bound_ticks));
    else
        ImGui::TextUnformatted("Water service bound: unknown");
    if (snapshot.fault_tick) {
        ImGui::Text("Fault at tick %llu", DisplayInteger(*snapshot.fault_tick));
        ImGui::TextUnformatted(snapshot.fault.c_str());
    }
    if (snapshot.hashes.valid && snapshot.hash_age_ticks) {
        ImGui::Text("Cached hashes at tick %llu; age %llu ticks",
                    DisplayInteger(snapshot.hashes.tick),
                    DisplayInteger(*snapshot.hash_age_ticks));
        ImGui::Text("world: %s", snapshot.hashes.world.c_str());
        ImGui::Text("regions: %s  creatures: %s",
                    snapshot.hashes.regions.c_str(),
                    snapshot.hashes.creatures.c_str());
        ImGui::Text(
            "items: %s  pages: %s", snapshot.hashes.items.c_str(), snapshot.hashes.pages.c_str());
    } else {
        ImGui::TextUnformatted("Cached hashes: unavailable");
    }
}
void DrawRows(const world::RegionPanelSnapshot& snapshot) {
    if (!ImGui::BeginTable("region_rows", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("Region");
    ImGui::TableSetupColumn("State / due");
    ImGui::TableSetupColumn("Cadence / phase");
    ImGui::TableSetupColumn("First / proximity / edit");
    ImGui::TableSetupColumn("Ticked / frozen / decision");
    ImGui::TableSetupColumn("Pressure / holds");
    ImGui::TableHeadersRow();
    const auto count = std::min(snapshot.rows.size(), world::RegionPanelSnapshot::kMaxRows);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& row = snapshot.rows[i];
        const auto& record = row.record;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d, %d", row.key.x, row.key.z);
        ImGui::TableNextColumn();
        ImGui::Text("%s / %s", StateName(record.state), row.due ? "due" : "idle");
        ImGui::TableNextColumn();
        ImGui::Text("2^%u / %u", static_cast<unsigned>(record.cadence_shift), record.cadence_phase);
        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu / %llu",
                    DisplayInteger(record.first_active),
                    DisplayInteger(record.last_proximity),
                    DisplayInteger(record.last_edit));
        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu / %llu",
                    DisplayInteger(record.last_ticked),
                    DisplayInteger(record.frozen_at),
                    DisplayInteger(record.last_decision));
        ImGui::TableNextColumn();
        ImGui::Text(
            "%llu / %u / %u", DisplayInteger(record.pressure), record.over_hold, record.under_hold);
    }
    ImGui::EndTable();
}
void DrawTransitions(const world::RegionPanelSnapshot& snapshot) {
    if (!ImGui::CollapsingHeader("Recent region transitions", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const auto count = std::min(snapshot.transition_count, snapshot.transitions.size());
    ImGui::Text("Last %zu actual transitions (capacity 64)", count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& observation = snapshot.transitions[i];
        const auto& transition = observation.transition;
        ImGui::Text("%llu: (%d, %d) %s -> %s",
                    DisplayInteger(observation.tick),
                    transition.key.x,
                    transition.key.z,
                    StateName(transition.from),
                    StateName(transition.to));
    }
}
} // namespace

bool DrawRegionPanel(const world::RegionPanelSnapshot& snapshot, RegionPanelState& state) {
    if (!state.open || !ImGui::GetCurrentContext())
        return false;
    state.page = snapshot.page;
    state.rows_per_page = snapshot.rows_per_page;
    state.descending = snapshot.descending;
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Regions (F4)", &state.open, ImGuiWindowFlags_NoSavedSettings)) {
        if (!snapshot.enabled) {
            ImGui::TextUnformatted("Active regions disabled");
        } else {
            DrawSummary(snapshot);
            ImGui::BeginDisabled(snapshot.page == 0);
            if (ImGui::Button("Previous page"))
                state.page = snapshot.page - 1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(snapshot.page + 1 >= snapshot.page_count);
            if (ImGui::Button("Next page"))
                state.page = snapshot.page + 1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("Page %zu / %zu (%zu regions)",
                        snapshot.page + 1,
                        snapshot.page_count,
                        snapshot.total_rows);
            ImGui::SameLine();
            if (ImGui::Checkbox("Descending", &state.descending))
                state.page = 0;
            constexpr const char* sizes[] = {"16", "64", "256"};
            int selected = snapshot.rows_per_page <= 16 ? 0 : snapshot.rows_per_page <= 64 ? 1 : 2;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("Rows per page", &selected, sizes, 3)) {
                constexpr std::size_t counts[] = {16, 64, 256};
                state.rows_per_page = counts[static_cast<std::size_t>(selected)];
                state.page = 0;
            }
            DrawRows(snapshot);
            DrawTransitions(snapshot);
        }
    }
    ImGui::End();
    return true;
}
} // namespace Luminumbra::Client::App
