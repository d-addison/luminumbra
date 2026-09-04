#include "UIHotReload.h"
#include "core/Log.h"
#include <algorithm>
#include <filesystem>

namespace Luminumbra::Client::UI {

UIHotReload::UIHotReload() {
    m_lastCheck = std::chrono::steady_clock::now();
}

UIHotReload::~UIHotReload() {
    ClearAllWatches();
}

void UIHotReload::WatchFile(const std::string& filePath) {
    if (!FileExists(filePath)) {
        LUMINUMBRA_CORE_WARN("[UI HotReload] File does not exist: {}", filePath);
        return;
    }

    WatchedFile watchedFile;
    watchedFile.path = filePath;
    watchedFile.lastWriteTime = GetFileWriteTime(filePath);
    watchedFile.isDirectory = false;

    m_watchedFiles[filePath] = watchedFile;

    LUMINUMBRA_CORE_INFO("[UI HotReload] Watching file: {}", filePath);
}

void UIHotReload::WatchDirectory(const std::string& directoryPath, const std::string& extension) {
    if (!std::filesystem::exists(directoryPath) || !std::filesystem::is_directory(directoryPath)) {
        LUMINUMBRA_CORE_WARN("[UI HotReload] Directory does not exist: {}", directoryPath);
        return;
    }

    WatchedFile watchedDir;
    watchedDir.path = directoryPath;
    watchedDir.lastWriteTime = GetFileWriteTime(directoryPath);
    watchedDir.isDirectory = true;
    watchedDir.extension = extension;

    m_watchedFiles[directoryPath] = watchedDir;

    // Also watch existing files in the directory
    ScanDirectory(directoryPath, extension);

    LUMINUMBRA_CORE_INFO("[UI HotReload] Watching directory: {} (extension: {})",
                         directoryPath,
                         extension.empty() ? "*" : extension);
}

void UIHotReload::StopWatching(const std::string& path) {
    auto it = m_watchedFiles.find(path);
    if (it != m_watchedFiles.end()) {
        m_watchedFiles.erase(it);
        LUMINUMBRA_CORE_INFO("[UI HotReload] Stopped watching: {}", path);
    }
}

void UIHotReload::ClearAllWatches() {
    m_watchedFiles.clear();
    LUMINUMBRA_CORE_INFO("[UI HotReload] Cleared all watches");
}

void UIHotReload::Update() {
    if (!m_enabled || m_watchedFiles.empty()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - m_lastCheck < CHECK_INTERVAL) {
        return;
    }

    m_lastCheck = now;

    // Check all watched files/directories
    std::vector<std::string> changedFiles;

    for (auto& [path, watchedFile] : m_watchedFiles) {
        if (CheckFileChanged(watchedFile)) {
            if (watchedFile.isDirectory) {
                // For directories, scan for changed files
                ScanDirectory(path, watchedFile.extension);
            } else {
                // File directly changed
                changedFiles.push_back(path);
            }
        }
    }

    // Trigger reloads for changed files
    for (const auto& filePath : changedFiles) {
        TriggerReload(filePath);
    }
}

void UIHotReload::TriggerReload(const std::string& filePath) {
    if (m_reloadCallback) {
        LUMINUMBRA_CORE_INFO("[UI HotReload] File changed, triggering reload: {}", filePath);
        m_reloadCallback(filePath);
    }
}

void UIHotReload::ReloadAll() {
    if (m_reloadCallback) {
        LUMINUMBRA_CORE_INFO("[UI HotReload] Manual reload all triggered");
        m_reloadCallback(""); // Empty path indicates reload all
    }
}

bool UIHotReload::CheckFileChanged(WatchedFile& watchedFile) {
    if (!FileExists(watchedFile.path)) {
        return false;
    }

    auto currentWriteTime = GetFileWriteTime(watchedFile.path);

    if (currentWriteTime != watchedFile.lastWriteTime) {
        watchedFile.lastWriteTime = currentWriteTime;
        return true;
    }

    return false;
}

void UIHotReload::ScanDirectory(const std::string& directoryPath, const std::string& extension) {
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filePath = entry.path().string();

                // Filter by extension if specified
                if (!extension.empty()) {
                    std::string fileExt = entry.path().extension().string();
                    if (fileExt != extension && fileExt != ("." + extension)) {
                        continue;
                    }
                }

                // Check if we're already watching this file individually
                auto it = m_watchedFiles.find(filePath);
                if (it == m_watchedFiles.end()) {
                    // Add to watch list
                    WatchedFile watchedFile;
                    watchedFile.path = filePath;
                    watchedFile.lastWriteTime = GetFileWriteTime(filePath);
                    watchedFile.isDirectory = false;

                    m_watchedFiles[filePath] = watchedFile;
                } else {
                    // Check if existing watched file changed
                    if (CheckFileChanged(it->second)) {
                        TriggerReload(filePath);
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LUMINUMBRA_CORE_ERROR(
            "[UI HotReload] Error scanning directory {}: {}", directoryPath, e.what());
    }
}

std::filesystem::file_time_type UIHotReload::GetFileWriteTime(const std::string& filePath) {
    try {
        return std::filesystem::last_write_time(filePath);
    } catch (const std::filesystem::filesystem_error&) {
        return std::filesystem::file_time_type::min();
    }
}

bool UIHotReload::FileExists(const std::string& filePath) {
    return std::filesystem::exists(filePath);
}

} // namespace Luminumbra::Client::UI