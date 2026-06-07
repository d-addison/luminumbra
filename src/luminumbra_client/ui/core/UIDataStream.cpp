#include "UIDataStream.h"
#include <imgui.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace Luminumbra::Client::UI {

UIDataStream::UIDataStream() {
    m_dirty_streams.reserve(MAX_DIRTY_STREAMS);
    LUMINUMBRA_CORE_INFO("[UIDataStream] Data streaming system initialized");
}

UIDataStream::~UIDataStream() {
    ClearAllStreams();
    LUMINUMBRA_CORE_INFO("[UIDataStream] Data streaming system shutdown");
}

void UIDataStream::ShowDebugWindow(bool* p_open) {
    if (!p_open || !*p_open) return;
    
    if (ImGui::Begin("Data Stream Debug", p_open)) {
        // Performance statistics
        ImGui::Text("Stream Performance");
        ImGui::Separator();
        ImGui::Text("Total Bindings: %zu", m_stats.total_bindings);
        ImGui::Text("Active Bindings: %zu", m_stats.active_bindings);
        ImGui::Text("Updates Per Frame: %zu", m_stats.updates_per_frame);
        ImGui::Text("Average Update Time: %.3f ms", m_stats.average_update_time_ms);
        ImGui::Text("Max Update Time: %.3f ms", m_stats.max_update_time_ms);
        
        // Performance bar
        float performance_ratio = m_stats.average_update_time_ms / 16.0f; // Target 16ms frame time
        ImVec4 color = performance_ratio < 0.1f ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : 
                      performance_ratio < 0.3f ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) : 
                      ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        
        ImGui::ProgressBar(performance_ratio, ImVec2(-1, 0), nullptr);
        ImGui::SameLine();
        ImGui::TextColored(color, "%.1f%% of frame budget", performance_ratio * 100.0f);
        
        ImGui::Spacing();
        
        // Stream list with search filter
        static char search_filter[256] = "";
        ImGui::Text("Stream List");
        ImGui::InputText("Filter", search_filter, sizeof(search_filter));
        ImGui::Separator();
        
        if (ImGui::BeginTable("StreamTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | 
                             ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY, ImVec2(0, 300))) {
            
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupColumn("Changed", ImGuiTableColumnFlags_NoSort);
            ImGui::TableHeadersRow();
            
            std::lock_guard<std::mutex> lock(m_bindings_mutex);
            
            // Create sorted list of stream entries
            std::vector<std::pair<std::string, IDataBinding*>> sorted_bindings;
            for (const auto& [id, binding] : m_bindings) {
                if (strlen(search_filter) == 0 || 
                    id.find(search_filter) != std::string::npos) {
                    sorted_bindings.emplace_back(id, binding.get());
                }
            }
            
            // Sort if requested
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsDirty) {
                    std::sort(sorted_bindings.begin(), sorted_bindings.end(),
                        [&](const auto& a, const auto& b) {
                            if (sort_specs->Specs->SortDirection == ImGuiSortDirection_Ascending) {
                                return a.first < b.first;
                            } else {
                                return a.first > b.first;
                            }
                        });
                    sort_specs->SpecsDirty = false;
                }
            }
            
            // Display entries
            for (const auto& [id, binding] : sorted_bindings) {
                ImGui::TableNextRow();
                
                // ID column
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(id.c_str());
                
                // Type column
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(binding->GetTypeIndex().name());
                
                // Value column
                ImGui::TableNextColumn();
                std::string value_str = binding->GetValueAsString();
                if (value_str.length() > 50) {
                    value_str = value_str.substr(0, 47) + "...";
                }
                ImGui::TextUnformatted(value_str.c_str());
                
                // Changed column
                ImGui::TableNextColumn();
                bool has_changed = binding->HasChanged();
                if (has_changed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Yes");
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No");
                }
            }
            
            ImGui::EndTable();
        }
        
        ImGui::Spacing();
        
        // Control buttons
        if (ImGui::Button("Clear All Streams")) {
            ClearAllStreams();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Log Stream State")) {
            LogStreamState();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Force Update")) {
            FlushUpdates();
        }
        
        // Batch mode indicator
        if (m_batch_mode) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "BATCH MODE ACTIVE");
            ImGui::Text("Queued updates: %zu", m_batch_updates.size());
        }
    }
    ImGui::End();
}

void UIDataStream::LogStreamState() const {
    std::lock_guard<std::mutex> lock(m_bindings_mutex);
    
    LUMINUMBRA_CORE_INFO("[UIDataStream] === Stream State Report ===");
    LUMINUMBRA_CORE_INFO("[UIDataStream] Total bindings: {}", m_stats.total_bindings);
    LUMINUMBRA_CORE_INFO("[UIDataStream] Active bindings: {}", m_stats.active_bindings);
    LUMINUMBRA_CORE_INFO("[UIDataStream] Updates per frame: {}", m_stats.updates_per_frame);
    LUMINUMBRA_CORE_INFO("[UIDataStream] Average update time: {:.3f} ms", m_stats.average_update_time_ms);
    
    for (const auto& [id, binding] : m_bindings) {
        std::string value = binding->GetValueAsString();
        bool changed = binding->HasChanged();
        LUMINUMBRA_CORE_INFO("[UIDataStream] Stream '{}': {} (changed: {})", 
                            id, value, changed ? "yes" : "no");
    }
    
    LUMINUMBRA_CORE_INFO("[UIDataStream] === End Report ===");
}

} // namespace Luminumbra::Client::UI