#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Luminumbra::Client::UI {

/**
 * Hot-reload system for UI development
 * Monitors file changes and triggers reloads automatically
 */
class UIHotReload {
public:
    // Callback paths are UTF-8 presentation strings; filesystem operations stay native.
    using ReloadCallback = std::function<void(const std::string& filePath)>;

    UIHotReload();
    ~UIHotReload();

    // Configuration
    void SetEnabled(bool enabled) {
        m_enabled = enabled;
    }
    bool IsEnabled() const {
        return m_enabled;
    }

    // File watching
    void WatchFile(const std::filesystem::path& filePath);
    void WatchDirectory(const std::filesystem::path& directoryPath,
                        const std::string& extension = "");
    void StopWatching(const std::filesystem::path& path);
    void ClearAllWatches();

    // Callbacks
    void SetReloadCallback(ReloadCallback callback) {
        m_reloadCallback = std::move(callback);
    }

    // Update (call from main loop)
    void Update();

    // Manual triggers
    void TriggerReload(const std::filesystem::path& filePath);
    void ReloadAll();

private:
    struct WatchedFile {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime;
    };

    bool m_enabled = false;
    std::unordered_map<std::filesystem::path, WatchedFile> m_watchedFiles;
    std::unordered_map<std::filesystem::path, std::unordered_set<std::string>> m_watchedDirectories;
    ReloadCallback m_reloadCallback;

    std::chrono::steady_clock::time_point m_lastCheck;
    static constexpr std::chrono::milliseconds CHECK_INTERVAL{1000}; // Check every second

    // Internal methods
    bool CheckFileChanged(WatchedFile& watchedFile);
    void ScanDirectory(const std::filesystem::path& directoryPath,
                       const std::unordered_set<std::string>& extensions,
                       std::vector<std::filesystem::path>* added = nullptr);
    std::filesystem::file_time_type GetFileWriteTime(const std::filesystem::path& filePath);
    bool FileExists(const std::filesystem::path& filePath);
};

} // namespace Luminumbra::Client::UI