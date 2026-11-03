#include "UIHotReload.h"
#include "core/FilesystemPath.h"
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

void UIHotReload::WatchFile(const std::filesystem::path& filePath) {
    if (!FileExists(filePath)) {
        LUMINUMBRA_CORE_WARN("[UI HotReload] File does not exist: {}",
                             Filesystem::GenericUtf8PathString(filePath));
        return;
    }

    WatchedFile watchedFile;
    watchedFile.path = filePath;
    watchedFile.lastWriteTime = GetFileWriteTime(filePath);

    m_watchedFiles[filePath] = watchedFile;

    LUMINUMBRA_CORE_INFO("[UI HotReload] Watching file: {}",
                         Filesystem::GenericUtf8PathString(filePath));
}

void UIHotReload::WatchDirectory(const std::filesystem::path& directoryPath,
                                 const std::string& extension) {
    std::error_code error;
    if (!std::filesystem::is_directory(directoryPath, error)) {
        LUMINUMBRA_CORE_WARN("[UI HotReload] Directory does not exist: {}",
                             Filesystem::GenericUtf8PathString(directoryPath));
        return;
    }
    auto& extensions = m_watchedDirectories[directoryPath];
    extensions.insert(extension);
    ScanDirectory(directoryPath, extensions);

    LUMINUMBRA_CORE_INFO("[UI HotReload] Watching directory: {} (extension: {})",
                         Filesystem::GenericUtf8PathString(directoryPath),
                         extension.empty() ? "*" : extension);
}

void UIHotReload::StopWatching(const std::filesystem::path& path) {
    m_watchedFiles.erase(path);
    m_watchedDirectories.erase(path);
    LUMINUMBRA_CORE_INFO("[UI HotReload] Stopped watching: {}",
                         Filesystem::GenericUtf8PathString(path));
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

    std::vector<std::filesystem::path> changedFiles;
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

void UIHotReload::TriggerReload(const std::filesystem::path& filePath) {
    if (m_reloadCallback) {
        LUMINUMBRA_CORE_INFO("[UI HotReload] File changed, triggering reload: {}",
                             Filesystem::GenericUtf8PathString(filePath));
        m_reloadCallback(Filesystem::GenericUtf8PathString(filePath));
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

void UIHotReload::ScanDirectory(const std::filesystem::path& directoryPath,
                                const std::unordered_set<std::string>& extensions,
                                std::vector<std::filesystem::path>* added) {
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
            if (!entry.is_regular_file())
                continue;
            const auto extension = Filesystem::GenericUtf8PathString(entry.path().extension());
            const auto bare = extension.empty() ? extension : extension.substr(1);
            if (!extensions.contains("") && !extensions.contains(extension) &&
                !extensions.contains(bare))
                continue;
            const auto path = entry.path();
            if (m_watchedFiles.contains(path))
                continue;
            m_watchedFiles.emplace(path, WatchedFile{path, GetFileWriteTime(path)});
            if (added)
                added->push_back(path);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LUMINUMBRA_CORE_ERROR("[UI HotReload] Error scanning directory {}: {}",
                              Filesystem::GenericUtf8PathString(directoryPath),
                              e.what());
    }
}

std::filesystem::file_time_type
UIHotReload::GetFileWriteTime(const std::filesystem::path& filePath) {
    try {
        return std::filesystem::last_write_time(filePath);
    } catch (const std::filesystem::filesystem_error&) {
        return std::filesystem::file_time_type::min();
    }
}

bool UIHotReload::FileExists(const std::filesystem::path& filePath) {
    std::error_code error;
    return std::filesystem::exists(filePath, error);
}

} // namespace Luminumbra::Client::UI