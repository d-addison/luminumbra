#pragma once

#include "Model.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace Luminumbra::Rendering {

class ModelManager {
public:
    static ModelManager& getInstance();

    // Gets a model from the cache or loads it if it's not present.
    std::shared_ptr<Model> getModel(const std::string& path);

private:
    ModelManager() = default;
    ~ModelManager() = default;
    ModelManager(const ModelManager&) = delete;
    ModelManager& operator=(const ModelManager&) = delete;

    std::unordered_map<std::string, std::shared_ptr<Model>> m_ModelCache;
};

} // namespace Luminumbra::Rendering