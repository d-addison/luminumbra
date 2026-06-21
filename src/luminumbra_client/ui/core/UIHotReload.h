#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <functional>

namespace Luminumbra::Client::UI {

/**
 * Hot-reload system for UI development
 * Monitors file changes and triggers reloads automatically
 */
class UIHotReload {
public:
    using ReloadCallback = std::function<void(const std::string& filePath)>;

    UIHotReload();
    ~UIHotReload();
    
    // Configuration
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    
    // File watching
    void WatchFile(const std::string& filePath);
    void WatchDirectory(const std::string& directoryPath, const std::string& extension = "");
    void StopWatching(const std::string& path);
    void ClearAllWatches();
    
    // Callbacks
    void SetReloadCallback(ReloadCallback callback) { m_reloadCallback = std::move(callback); }
    
    // Update (call from main loop)
    void Update();
    
    // Manual triggers
    void TriggerReload(const std::string& filePath);
    void ReloadAll();

private:
    struct WatchedFile {
        std::string path;
        std::filesystem::file_time_type lastWriteTime;
        bool isDirectory;
        std::string extension; // For directory watches
    };
    
    bool m_enabled = false;
    std::unordered_map<std::string, WatchedFile> m_watchedFiles;
    ReloadCallback m_reloadCallback;
    
    std::chrono::steady_clock::time_point m_lastCheck;
    static constexpr std::chrono::milliseconds CHECK_INTERVAL{1000}; // Check every second
    
    // Internal methods
    bool CheckFileChanged(WatchedFile& watchedFile);
    void ScanDirectory(const std::string& directoryPath, const std::string& extension);
    std::filesystem::file_time_type GetFileWriteTime(const std::string& filePath);
    bool FileExists(const std::string& filePath);
};

} // namespace Luminumbra::Client::UI