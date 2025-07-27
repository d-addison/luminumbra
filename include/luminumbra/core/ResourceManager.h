#pragma once

#include <string>
#include <vector>

namespace Luminumbra::Core {

class ResourceManager {
public:
    // Deleted constructor to enforce singleton pattern
    ResourceManager(ResourceManager const&) = delete;
    void operator=(ResourceManager const&) = delete;

    // Public accessor for the singleton instance
    static ResourceManager& getInstance();

    // Initializes the resource manager with the base path of the executable
    static void init(const std::string& executablePath);

    // Resolves a relative resource path to a full path
    std::string getResourcePath(const std::string& relativePath);

private:
    // Private constructor for singleton pattern
    ResourceManager() {}

    // The base path for all resources
    static std::string m_BasePath;
};

} // namespace Luminumbra::Core
