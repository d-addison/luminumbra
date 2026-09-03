#include "WorldList.h"
#include "core/Log.h"
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace Luminumbra::Client::UI {

namespace {

std::optional<std::chrono::sys_days> ParseIsoDate(const std::string& timestamp) {
    if (timestamp.size() < 10 || timestamp[4] != '-' || timestamp[7] != '-') {
        return std::nullopt;
    }

    int yearValue = 0;
    unsigned monthValue = 0;
    unsigned dayValue = 0;
    const auto parsePart = [&](std::size_t offset, std::size_t count, auto& value) {
        const char* begin = timestamp.data() + offset;
        const char* end = begin + count;
        const auto result = std::from_chars(begin, end, value);
        return result.ec == std::errc{} && result.ptr == end;
    };
    if (!parsePart(0, 4, yearValue) || !parsePart(5, 2, monthValue) || !parsePart(8, 2, dayValue)) {
        return std::nullopt;
    }

    const std::chrono::year_month_day date{
        std::chrono::year{yearValue}, std::chrono::month{monthValue}, std::chrono::day{dayValue}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date};
}

std::string EscapeRml(const std::string& value) {
    return Rml::StringUtilities::EncodeRml(value);
}

} // namespace

WorldList::WorldList(const std::string& elementId)
    : UIComponent(elementId) {}

void WorldList::OnElementSet() {
    if (!m_element)
        return;

    // Find child elements
    if (auto* parent = m_element->GetParentNode()) {
        m_loadingElement = parent->GetElementById("loading_worlds");
        m_emptyElement = parent->GetElementById("no_worlds");
    }

    m_listContainer = m_element->GetElementById("world_list_items");
    if (!m_listContainer) {
        // Create list container if it doesn't exist
        auto listContainer = m_document->CreateElement("div");
        m_listContainer = listContainer.get();
        m_listContainer->SetId("world_list_items");
        m_element->AppendChild(std::move(listContainer));
    }

    UpdateLoadingState();
    UpdateEmptyState();
}

void WorldList::Update(float deltaTime) {
    (void)deltaTime;
    UpdateLoadingState();
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
        std::remove_if(m_worlds.begin(),
                       m_worlds.end(),
                       [&worldId](const WorldInfo& world) { return world.id == worldId; }),
        m_worlds.end());

    if (m_selectedWorldId == worldId) {
        ClearSelection();
    }

    ApplyFilters();
    RebuildList();
    UpdateEmptyState();
}

void WorldList::SetLoading(bool loading) {
    if (m_loading != loading) {
        m_loading = loading;
        UpdateLoadingState();
        UpdateEmptyState();
    }
}

void WorldList::UpdateWorld(const WorldInfo& world) {
    auto it = std::find_if(m_worlds.begin(), m_worlds.end(), [&world](const WorldInfo& w) {
        return w.id == world.id;
    });

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
        PruneDetachedEventListeners();
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
                std::string itemWorldId = item->GetAttribute<Rml::String>("data-world-id", "");
                if (itemWorldId == worldId) {
                    item->SetClass("selected", true);
                } else {
                    item->SetClass("selected", false);
                }
            }
        }

        // Find selected world and call callback
        auto it = std::find_if(m_worlds.begin(),
                               m_worlds.end(),
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
    if (m_selectedWorldId.empty())
        return nullptr;

    auto it = std::find_if(m_worlds.begin(), m_worlds.end(), [this](const WorldInfo& world) {
        return world.id == m_selectedWorldId;
    });

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
    TrackSubscription(worldsProperty,
                      [this](const std::vector<WorldInfo>& oldWorlds,
                             const std::vector<WorldInfo>& newWorlds) { SetWorlds(newWorlds); });
}

void WorldList::BindSelectedWorldId(Property<std::string>& selectedIdProperty) {
    // Set initial selection
    if (!selectedIdProperty.Get().empty()) {
        SelectWorld(selectedIdProperty.Get());
    }

    // Subscribe to property changes
    TrackSubscription(selectedIdProperty,
                      [this](const std::string& oldId, const std::string& newId) {
                          if (!newId.empty()) {
                              SelectWorld(newId);
                          } else {
                              ClearSelection();
                          }
                      });

    // Update property when selection changes
    SetSelectionCallback(
        [&selectedIdProperty](const WorldInfo& world) { selectedIdProperty.Set(world.id); });
}

void WorldList::BindSearchFilter(Property<std::string>& searchProperty) {
    // Set initial filter
    SetSearchFilter(searchProperty.Get());

    // Subscribe to changes
    TrackSubscription(searchProperty,
                      [this](const std::string& oldFilter, const std::string& newFilter) {
                          SetSearchFilter(newFilter);
                      });
}

void WorldList::BindLoading(Property<bool>& loadingProperty) {
    SetLoading(loadingProperty.Get());
    TrackSubscription(loadingProperty,
                      [this](const bool&, const bool& loading) { SetLoading(loading); });
}

void WorldList::RebuildList() {
    if (!m_listContainer)
        return;

    // Clear existing items
    m_listContainer->SetInnerRML("");
    PruneDetachedEventListeners();

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
    std::sort(m_filteredWorlds.begin(),
              m_filteredWorlds.end(),
              [this](const WorldInfo& a, const WorldInfo& b) { return CompareWorlds(a, b); });
}

void WorldList::CreateWorldElement(const WorldInfo& world) {
    if (!m_listContainer || !m_document)
        return;

    // Create main list item
    auto itemHandle = m_document->CreateElement("div");
    Rml::Element* item = itemHandle.get();
    item->SetClass("list-item", true);
    item->SetAttribute("data-world-id", world.id);

    // Create world entry content
    std::stringstream content;
    content << "<div class=\"world-entry-header\">";
    content << "<h3 class=\"list-item-title\">" << EscapeRml(world.name) << "</h3>";
    content << "<div class=\"world-actions\">";
    content << "<button class=\"btn btn-sm btn-secondary world-favorite\" data-favorite=\""
            << (world.favorite ? "true" : "false") << "\" data-world-id=\"" << EscapeRml(world.id)
            << "\">" << (world.favorite ? "★" : "☆") << "</button>";
    content << "<button class=\"btn btn-sm btn-danger world-delete\" data-world-id=\""
            << EscapeRml(world.id) << "\">Delete</button>";
    content << "</div>";
    content << "</div>";

    content << "<p class=\"list-item-description\">";
    content << "Type: " << EscapeRml(GetWorldTypeDisplayName(world.type));
    if (!world.seed.empty()) {
        content << " | Seed: " << EscapeRml(world.seed);
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
    AddTrackedEventListener(
        item, "click", [this, worldId = world.id](Rml::Event&) { HandleWorldClick(worldId); });

    // Add favorite button handler
    if (auto* favoriteBtn = item->QuerySelector(".world-favorite")) {
        AddTrackedEventListener(
            favoriteBtn, "click", [this, worldId = world.id](Rml::Event& event) {
                event.StopPropagation();
                HandleFavoriteToggle(worldId);
            });
    }

    // Add delete button handler
    if (auto* deleteBtn = item->QuerySelector(".world-delete")) {
        AddTrackedEventListener(deleteBtn, "click", [this, worldId = world.id](Rml::Event& event) {
            event.StopPropagation();
            HandleWorldDelete(worldId);
        });
    }

    // Check if this world is selected
    if (world.id == m_selectedWorldId) {
        item->SetClass("selected", true);
    }

    m_listContainer->AppendChild(std::move(itemHandle));
}

void WorldList::UpdateLoadingState() {
    if (m_loadingElement) {
        m_loadingElement->SetProperty("display", m_loading ? "block" : "none");
    }
    if (m_listContainer) {
        m_listContainer->SetProperty("display", m_loading ? "none" : "block");
    }
}

void WorldList::UpdateEmptyState() {
    if (m_emptyElement) {
        const bool isEmpty = !m_loading && m_filteredWorlds.empty();
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
    const auto world =
        std::find_if(m_worlds.begin(), m_worlds.end(), [&worldId](const WorldInfo& candidate) {
            return candidate.id == worldId;
        });
    if (world == m_worlds.end()) {
        return;
    }
    world->favorite = !world->favorite;
    ApplyFilters();
    RebuildList();
    UpdateEmptyState();
}

bool WorldList::MatchesSearchFilter(const WorldInfo& world) const {
    std::string lowerFilter = m_searchFilter;
    std::transform(lowerFilter.begin(),
                   lowerFilter.end(),
                   lowerFilter.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    std::string lowerName = world.name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });

    return lowerName.find(lowerFilter) != std::string::npos;
}

bool WorldList::MatchesTypeFilter(const WorldInfo& world) const {
    return world.type == m_typeFilter;
}

bool WorldList::MatchesFavoriteFilter(const WorldInfo& world) const {
    return world.favorite;
}

bool WorldList::MatchesRecentFilter(const WorldInfo& world) const {
    const auto played = ParseIsoDate(world.lastPlayed);
    if (!played) {
        return false;
    }
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const auto age = today - *played;
    return age >= std::chrono::days{0} && age <= std::chrono::days{7};
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
    const auto played = ParseIsoDate(timestamp);
    if (!played) {
        return timestamp.empty() ? "Never" : timestamp;
    }
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const auto age = today - *played;
    if (age == std::chrono::days{0}) {
        return "Today";
    }
    if (age == std::chrono::days{1}) {
        return "Yesterday";
    }
    return timestamp.substr(0, 10);
}

std::string WorldList::GetWorldTypeDisplayName(const std::string& type) const {
    static const std::unordered_map<std::string, std::string> typeNames = {
        {"default", "Default Terrain"},
        {"archipelago", "Archipelago"},
        {"mountains", "Mountainous"},
        {"flat_lands", "Flat Plains"},
        {"temperate_forest", "Temperate Forest"},
        {"desert", "Desert Wasteland"},
        {"frozen", "Frozen Tundra"}};

    auto it = typeNames.find(type);
    return (it != typeNames.end()) ? it->second : type;
}

bool WorldList::CompareWorlds(const WorldInfo& a, const WorldInfo& b) const {
    bool less = false;
    bool greater = false;
    switch (m_sortBy) {
        case SortBy::Name:
            less = a.name < b.name;
            greater = b.name < a.name;
            break;
        case SortBy::LastPlayed:
            less = a.lastPlayed < b.lastPlayed;
            greater = b.lastPlayed < a.lastPlayed;
            break;
        case SortBy::Size:
            less = a.fileSize < b.fileSize;
            greater = b.fileSize < a.fileSize;
            break;
        case SortBy::CreationDate:
            less = a.createdAt < b.createdAt;
            greater = b.createdAt < a.createdAt;
            break;
    }
    if (!less && !greater) {
        return m_sortAscending ? a.id < b.id : b.id < a.id;
    }
    return m_sortAscending ? less : greater;
}

} // namespace Luminumbra::Client::UI
