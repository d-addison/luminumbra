#include "luminumbra/core/ResourceManager.h"
#include <filesystem>

namespace Luminumbra::Core {

// Initialize static member
std::string ResourceManager::m_BasePath;

ResourceManager& ResourceManager::getInstance() {
    static ResourceManager instance;
    return instance;
}

void ResourceManager::init(const std::string& executablePath) {
    // Find the parent directory of the executable
    std::filesystem::path exePath(executablePath);
    m_BasePath = exePath.parent_path().parent_path().string();
}

std::string ResourceManager::getResourcePath(const std::string& relativePath) {
    // Combine the base path with the relative path
    std::filesystem::path fullPath = std::filesystem::path(m_BasePath) / relativePath;
    return fullPath.string();
}

} // namespace Luminumbra::Core
