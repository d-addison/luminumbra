#include "WorldList.h"
#include "core/Log.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace Luminumbra::Client::UI {

WorldList::WorldList(const std::string& elementId) : UIComponent(elementId) {
}

void WorldList::OnElementSet() {
    if (!m_element) return;
    
    // Find child elements
    if (auto* parent = m_element->GetParentNode()) {
        m_loadingElement = parent->GetElementById("loading_worlds");
        m_emptyElement = parent->GetElementById("no_worlds");
    }
    
    m_listContainer = m_element->GetElementById("world_list_items");
    if (!m_listContainer) {
        // Create list container if it doesn't exist
        m_listContainer = m_document->CreateElement("div");
        m_listContainer->SetId("world_list_items");
        m_element->AppendChild(m_listContainer);
    }
    
    UpdateLoadingState();
    UpdateEmptyState();
}

void WorldList::Update(float deltaTime) {
    // Handle any pending UI updates
}

void WorldList::SetWorlds(const std::vector<WorldInfo>& worlds) {
    m_worlds = worlds;
    ApplyFilters();
    RebuildList();
    UpdateEmptyState();
}

void WorldList::AddWorld(const WorldInfo& world) {
    m_worlds.push_back(world);
    ApplyFilters();
    RebuildList();
    UpdateEmptyState();
}

void WorldList::RemoveWorld(const std::string& worldId) {
    m_worlds.erase(
        std::remove_if(m_worlds.begin(), m_worlds.end(),
            [&worldId](const WorldInfo& world) { return world.id == worldId; }),
        m_worlds.end()
    );
    
    if (m_selectedWorldId == worldId) {
        ClearSelection();
    }
    
    ApplyFilters();
    RemoveWorldElement(worldId);
    UpdateEmptyState();
}

void WorldList::UpdateWorld(const WorldInfo& world) {
    auto it = std::find_if(m_worlds.begin(), m_worlds.end(),
        [&world](const WorldInfo& w) { return w.id == world.id; });
    
    if (it != m_worlds.end()) {
        *it = world;
        ApplyFilters();
        RebuildList();
    }
}

void WorldList::ClearWorlds() {
    m_worlds.clear();
    m_filteredWorlds.clear();
    ClearSelection();
    
    if (m_listContainer) {
        m_listContainer->SetInnerRML("");
    }
    
    UpdateEmptyState();
}

void WorldList::SelectWorld(const std::string& worldId) {
    if (m_selectedWorldId != worldId) {
        m_selectedWorldId = worldId;
        
        // Update visual selection
        if (m_listContainer) {
            Rml::ElementList items;
            m_listContainer->GetElementsByClassName(items, "list-item");
            
            for (auto* item : items) {
                std::string itemWorldId = item->GetAttribute("data-world-id", "");
                if (itemWorldId == worldId) {
                    item->SetClass("selected", true);
                } else {
                    item->SetClass("selected", false);
                }
            }
        }
        
        // Find selected world and call callback
        auto it = std::find_if(m_worlds.begin(), m_worlds.end(),
            [&worldId](const WorldInfo& world) { return world.id == worldId; });
        
        if (it != m_worlds.end() && m_selectionCallback) {
            m_selectionCallback(*it);
        }
    }
}

void WorldList::ClearSelection() {
    m_selectedWorldId.clear();
    
    if (m_listContainer) {
        Rml::ElementList items;
        m_listContainer->GetElementsByClassName(items, "list-item");
        
        for (auto* item : items) {
            item->SetClass("selected", false);
        }
    }
}

const WorldInfo* WorldList::GetSelectedWorld() const {
    if (m_selectedWorldId.empty()) return nullptr;
    
    auto it = std::find_if(m_worlds.begin(), m_worlds.end(),
        [this](const WorldInfo& world) { return world.id == m_selectedWorldId; });
    
    return (it != m_worlds.end()) ? &(*it) : nullptr;
}

void WorldList::SetSearchFilter(const std::string& filter) {
    if (m_searchFilter != filter) {
        m_searchFilter = filter;
        ApplyFilters();
        RebuildList();
    }
}

void WorldList::SetTypeFilter(const std::string& type) {
    if (m_typeFilter != type) {
        m_typeFilter = type;
        ApplyFilters();
        RebuildList();
    }
}

void WorldList::ShowFavoritesOnly(bool favoritesOnly) {
    if (m_showFavoritesOnly != favoritesOnly) {
        m_showFavoritesOnly = favoritesOnly;
        ApplyFilters();
        RebuildList();
    }
}

void WorldList::ShowRecentOnly(bool recentOnly) {
    if (m_showRecentOnly != recentOnly) {
        m_showRecentOnly = recentOnly;
        ApplyFilters();
        RebuildList();
    }
}

void WorldList::SetSortBy(SortBy sortBy, bool ascending) {
    if (m_sortBy != sortBy || m_sortAscending != ascending) {
        m_sortBy = sortBy;
        m_sortAscending = ascending;
        SortWorlds();
        RebuildList();
    }
}

void WorldList::BindWorlds(Property<std::vector<WorldInfo>>& worldsProperty) {
    // Set initial worlds
    SetWorlds(worldsProperty.Get());
    
    // Subscribe to changes
    worldsProperty.Subscribe([this](const std::vector<WorldInfo>& oldWorlds, const std::vector<WorldInfo>& newWorlds) {
        SetWorlds(newWorlds);
    });
}

void WorldList::BindSelectedWorldId(Property<std::string>& selectedIdProperty) {
    // Set initial selection
    if (!selectedIdProperty.Get().empty()) {
        SelectWorld(selectedIdProperty.Get());
    }
    
    // Subscribe to property changes
    selectedIdProperty.Subscribe([this](const std::string& oldId, const std::string& newId) {
        if (!newId.empty()) {
            SelectWorld(newId);
        } else {
            ClearSelection();
        }
    });
    
    // Update property when selection changes
    SetSelectionCallback([&selectedIdProperty](const WorldInfo& world) {
        selectedIdProperty.Set(world.id);
    });
}

void WorldList::BindSearchFilter(Property<std::string>& searchProperty) {
    // Set initial filter
    SetSearchFilter(searchProperty.Get());
    
    // Subscribe to changes
    searchProperty.Subscribe([this](const std::string& oldFilter, const std::string& newFilter) {
        SetSearchFilter(newFilter);
    });
}

void WorldList::RebuildList() {
    if (!m_listContainer) return;
    
    // Clear existing items
    m_listContainer->SetInnerRML("");
    
    // Hide loading state
    UpdateLoadingState();
    
    // Create elements for filtered worlds
    for (const auto& world : m_filteredWorlds) {
        CreateWorldElement(world);
    }
}

void WorldList::ApplyFilters() {
    m_filteredWorlds.clear();
    
    for (const auto& world : m_worlds) {
        bool matches = true;
        
        // Apply search filter
        if (!m_searchFilter.empty() && !MatchesSearchFilter(world)) {
            matches = false;
        }
        
        // Apply type filter
        if (!m_typeFilter.empty() && !MatchesTypeFilter(world)) {
            matches = false;
        }
        
        // Apply favorites filter
        if (m_showFavoritesOnly && !MatchesFavoriteFilter(world)) {
            matches = false;
        }
        
        // Apply recent filter
        if (m_showRecentOnly && !MatchesRecentFilter(world)) {
            matches = false;
        }
        
        if (matches) {
            m_filteredWorlds.push_back(world);
        }
    }
    
    // Sort filtered results
    SortWorlds();
}

void WorldList::SortWorlds() {
    std::sort(m_filteredWorlds.begin(), m_filteredWorlds.end(),
        [this](const WorldInfo& a, const WorldInfo& b) {
            return CompareWorlds(a, b);
        });
}

void WorldList::CreateWorldElement(const WorldInfo& world) {
    if (!m_listContainer || !m_document) return;
    
    // Create main list item
    Rml::Element* item = m_document->CreateElement("div");
    item->SetClass("list-item", true);
    item->SetAttribute("data-world-id", world.id);
    
    // Create world entry content
    std::stringstream content;
    content << "<div class=\"world-entry-header\">";
    content << "<h3 class=\"list-item-title\">" << world.name << "</h3>";
    content << "<div class=\"world-actions\">";
    content << "<button class=\"btn btn-sm btn-secondary world-favorite\" data-favorite=\"false\" data-world-id=\"" << world.id << "\">☆</button>";
    content << "<button class=\"btn btn-sm btn-danger world-delete\" data-world-id=\"" << world.id << "\">Delete</button>";
    content << "</div>";
    content << "</div>";
    
    content << "<p class=\"list-item-description\">";
    content << "Type: " << GetWorldTypeDisplayName(world.type);
    if (!world.seed.empty()) {
        content << " | Seed: " << world.seed;
    }
    content << "<br/>";
    content << "Last played: " << FormatLastPlayed(world.lastPlayed);
    content << " | " << FormatFileSize(world.fileSize);
    content << "</p>";
    
    content << "<div class=\"world-preview\">";
    content << "<div class=\"world-thumbnail\"></div>";
    content << "</div>";
    
    item->SetInnerRML(content.str());
    
    // Add event handlers
    auto clickListener = std::make_unique<LambdaEventListener>([this, worldId = world.id](Rml::Event&) {
        HandleWorldClick(worldId);
    });
    
    item->AddEventListener("click", clickListener.get());
    m_eventListeners.emplace_back(std::move(clickListener));
    
    // Add favorite button handler
    if (auto* favoriteBtn = item->GetElementById("world-favorite")) {
        auto favoriteListener = std::make_unique<LambdaEventListener>([this, worldId = world.id](Rml::Event& event) {
            event.StopPropagation();
            HandleFavoriteToggle(worldId);
        });
        
        favoriteBtn->AddEventListener("click", favoriteListener.get());
        m_eventListeners.emplace_back(std::move(favoriteListener));
    }
    
    // Add delete button handler
    if (auto* deleteBtn = item->GetElementById("world-delete")) {
        auto deleteListener = std::make_unique<LambdaEventListener>([this, worldId = world.id](Rml::Event& event) {
            event.StopPropagation();
            HandleWorldDelete(worldId);
        });
        
        deleteBtn->AddEventListener("click", deleteListener.get());
        m_eventListeners.emplace_back(std::move(deleteListener));
    }
    
    // Check if this world is selected
    if (world.id == m_selectedWorldId) {
        item->SetClass("selected", true);
    }
    
    m_listContainer->AppendChild(item);
}

void WorldList::RemoveWorldElement(const std::string& worldId) {
    if (!m_listContainer) return;
    
    Rml::ElementList items;
    m_listContainer->GetElementsByClassName(items, "list-item");
    
    for (auto* item : items) {
        if (item->GetAttribute("data-world-id", "") == worldId) {
            m_listContainer->RemoveChild(item);
            break;
        }
    }
}

void WorldList::UpdateLoadingState() {
    if (m_loadingElement) {
        bool isLoading = false; // TODO: Add loading state management
        m_loadingElement->SetProperty("display", isLoading ? "block" : "none");
    }
}

void WorldList::UpdateEmptyState() {
    if (m_emptyElement) {
        bool isEmpty = m_filteredWorlds.empty() && !m_worlds.empty(); // No filtered results
        m_emptyElement->SetProperty("display", isEmpty ? "block" : "none");
    }
}

void WorldList::HandleWorldClick(const std::string& worldId) {
    SelectWorld(worldId);
}

void WorldList::HandleWorldDelete(const std::string& worldId) {
    if (m_deletionCallback) {
        m_deletionCallback(worldId);
    }
}

void WorldList::HandleFavoriteToggle(const std::string& worldId) {
    // TODO: Implement favorite toggle functionality
    LUMINUMBRA_CORE_INFO("[UI] Toggle favorite for world: {}", worldId);
}

bool WorldList::MatchesSearchFilter(const WorldInfo& world) const {
    std::string lowerFilter = m_searchFilter;
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
    
    std::string lowerName = world.name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    
    return lowerName.find(lowerFilter) != std::string::npos;
}

bool WorldList::MatchesTypeFilter(const WorldInfo& world) const {
    return world.type == m_typeFilter;
}

bool WorldList::MatchesFavoriteFilter(const WorldInfo& world) const {
    // TODO: Implement favorite status tracking
    return true; // Placeholder
}

bool WorldList::MatchesRecentFilter(const WorldInfo& world) const {
    // TODO: Implement recent filter logic (e.g., played within last week)
    return true; // Placeholder
}

std::string WorldList::FormatFileSize(size_t bytes) const {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    
    while (size >= 1024.0 && unit < 3) {
        size /= 1024.0;
        unit++;
    }
    
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    return ss.str();
}

std::string WorldList::FormatLastPlayed(const std::string& timestamp) const {
    // TODO: Implement proper timestamp parsing and formatting
    return timestamp.empty() ? "Never" : timestamp;
}

std::string WorldList::GetWorldTypeDisplayName(const std::string& type) const {
    static const std::unordered_map<std::string, std::string> typeNames = {
        {"default", "Default Terrain"},
        {"archipelago", "Archipelago"},
        {"mountains", "Mountainous"},
        {"flat_lands", "Flat Plains"},
        {"temperate_forest", "Temperate Forest"},
        {"desert", "Desert Wasteland"},
        {"frozen", "Frozen Tundra"}
    };
    
    auto it = typeNames.find(type);
    return (it != typeNames.end()) ? it->second : type;
}

bool WorldList::CompareWorlds(const WorldInfo& a, const WorldInfo& b) const {
    bool result = false;
    
    switch (m_sortBy) {
        case SortBy::Name:
            result = a.name < b.name;
            break;
        case SortBy::LastPlayed:
            result = a.lastPlayed < b.lastPlayed;
            break;
        case SortBy::Size:
            result = a.fileSize < b.fileSize;
            break;
        case SortBy::CreationDate:
            // TODO: Add creation date to WorldInfo
            result = a.id < b.id; // Fallback to ID
            break;
    }
    
    return m_sortAscending ? result : !result;
}

} // namespace Luminumbra::Client::UI