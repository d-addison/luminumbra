#include "UIHotReload.h"
#include "core/Log.h"
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

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

    m_watchedFiles[filePath] = watchedFile;

    LUMINUMBRA_CORE_INFO("[UI HotReload] Watching file: {}", filePath);
}

void UIHotReload::WatchDirectory(const std::string& directoryPath, const std::string& extension) {
    std::error_code error;
    if (!std::filesystem::is_directory(directoryPath, error)) {
        LUMINUMBRA_CORE_WARN("[UI HotReload] Directory does not exist: {}", directoryPath);
        return;
    }
    auto& extensions = m_watchedDirectories[directoryPath];
    extensions.insert(extension);
    ScanDirectory(directoryPath, extensions);

    LUMINUMBRA_CORE_INFO("[UI HotReload] Watching directory: {} (extension: {})",
                         directoryPath,
                         extension.empty() ? "*" : extension);
}

void UIHotReload::StopWatching(const std::string& path) {
    m_watchedFiles.erase(path);
    m_watchedDirectories.erase(path);
    LUMINUMBRA_CORE_INFO("[UI HotReload] Stopped watching: {}", path);
}

void UIHotReload::ClearAllWatches() {
    m_watchedFiles.clear();
    m_watchedDirectories.clear();
    LUMINUMBRA_CORE_INFO("[UI HotReload] Cleared all watches");
}

void UIHotReload::Update() {
    if (!m_enabled || (m_watchedFiles.empty() && m_watchedDirectories.empty())) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - m_lastCheck < CHECK_INTERVAL) {
        return;
    }

    m_lastCheck = now;

    std::vector<std::string> changedFiles;
    // Scan separately: discovering files can rehash the file map. Scanning each
    // directory also discovers nested additions whose parent timestamp is unchanged.
    for (const auto& [path, extensions] : m_watchedDirectories)
        ScanDirectory(path, extensions, &changedFiles);
    for (auto& [path, watchedFile] : m_watchedFiles) {
        if (CheckFileChanged(watchedFile))
            changedFiles.push_back(path);
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

void UIHotReload::ScanDirectory(const std::string& directoryPath,
                                const std::unordered_set<std::string>& extensions,
                                std::vector<std::string>* added) {
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
            if (!entry.is_regular_file())
                continue;
            const auto extension = entry.path().extension().string();
            const auto bare = extension.empty() ? extension : extension.substr(1);
            if (!extensions.contains("") && !extensions.contains(extension) &&
                !extensions.contains(bare))
                continue;
            const auto path = entry.path().string();
            if (m_watchedFiles.contains(path))
                continue;
            m_watchedFiles.emplace(path, WatchedFile{path, GetFileWriteTime(path)});
            if (added)
                added->push_back(path);
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
    std::error_code error;
    return std::filesystem::exists(filePath, error);
}

} // namespace Luminumbra::Client::UI