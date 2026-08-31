#pragma once

#include "ui/core/UIComponent.h"
#include "ui/core/UIStateManager.h"
#include <vector>
#include <functional>

namespace Luminumbra::Client::UI {

/**
 * WorldList component for displaying and managing saved worlds
 */
class WorldList : public UIComponent {
public:
    using SelectionCallback = std::function<void(const WorldInfo&)>;
    using DeletionCallback = std::function<void(const std::string& worldId)>;

    explicit WorldList(const std::string& elementId);
    
    // Data management
    void SetWorlds(const std::vector<WorldInfo>& worlds);
    void AddWorld(const WorldInfo& world);
    void RemoveWorld(const std::string& worldId);
    void UpdateWorld(const WorldInfo& world);
    void ClearWorlds();
    void SetLoading(bool loading);

    // Selection
    void SelectWorld(const std::string& worldId);
    void ClearSelection();
    const WorldInfo* GetSelectedWorld() const;
    
    // Filtering and search
    void SetSearchFilter(const std::string& filter);
    void SetTypeFilter(const std::string& type);
    void ShowFavoritesOnly(bool favoritesOnly);
    void ShowRecentOnly(bool recentOnly);
    
    // Sorting
    enum class SortBy {
        Name,
        LastPlayed,
        Size,
        CreationDate
    };
    
    void SetSortBy(SortBy sortBy, bool ascending = true);
    
    // Events
    void SetSelectionCallback(SelectionCallback callback) { m_selectionCallback = std::move(callback); }
    void SetDeletionCallback(DeletionCallback callback) { m_deletionCallback = std::move(callback); }
    
    // Data binding
    void BindWorlds(Property<std::vector<WorldInfo>>& worldsProperty);
    void BindSelectedWorldId(Property<std::string>& selectedIdProperty);
    void BindSearchFilter(Property<std::string>& searchProperty);
    void BindLoading(Property<bool>& loadingProperty);

    // State
    bool IsEmpty() const { return m_filteredWorlds.empty(); }
    size_t GetWorldCount() const { return m_worlds.size(); }
    size_t GetFilteredWorldCount() const { return m_filteredWorlds.size(); }

protected:
    void OnElementSet() override;
    void Update(float deltaTime) override;

private:
    std::vector<WorldInfo> m_worlds;
    std::vector<WorldInfo> m_filteredWorlds;
    std::string m_selectedWorldId;
    
    // Filter settings
    std::string m_searchFilter;
    std::string m_typeFilter;
    bool m_showFavoritesOnly = false;
    bool m_showRecentOnly = false;
    bool m_loading = false;

    // Sort settings
    SortBy m_sortBy = SortBy::LastPlayed;
    bool m_sortAscending = false;
    
    // Callbacks
    SelectionCallback m_selectionCallback;
    DeletionCallback m_deletionCallback;
    
    // UI elements
    Rml::Element* m_listContainer = nullptr;
    Rml::Element* m_loadingElement = nullptr;
    Rml::Element* m_emptyElement = nullptr;
    
    // Internal methods
    void RebuildList();
    void ApplyFilters();
    void SortWorlds();
    void CreateWorldElement(const WorldInfo& world);
    void UpdateLoadingState();
    void UpdateEmptyState();
    
    void HandleWorldClick(const std::string& worldId);
    void HandleWorldDelete(const std::string& worldId);
    void HandleFavoriteToggle(const std::string& worldId);
    
    // Utility methods
    bool MatchesSearchFilter(const WorldInfo& world) const;
    bool MatchesTypeFilter(const WorldInfo& world) const;
    bool MatchesFavoriteFilter(const WorldInfo& world) const;
    bool MatchesRecentFilter(const WorldInfo& world) const;
    
    std::string FormatFileSize(size_t bytes) const;
    std::string FormatLastPlayed(const std::string& timestamp) const;
    std::string GetWorldTypeDisplayName(const std::string& type) const;
    
    // Sorting comparisons
    bool CompareWorlds(const WorldInfo& a, const WorldInfo& b) const;
};

} // namespace Luminumbra::Client::UI
