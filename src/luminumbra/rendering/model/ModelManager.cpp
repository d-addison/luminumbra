#include "luminumbra/rendering/model/ModelManager.h"
#include "luminumbra/core/Debug.h"

namespace Luminumbra::Rendering {

ModelManager& ModelManager::getInstance() {
    static ModelManager instance;
    return instance;
}

std::shared_ptr<Model> ModelManager::getModel(const std::string& path) {
    auto it = m_ModelCache.find(path);
    if (it != m_ModelCache.end()) {
        return it->second;
    }

    LOG("ModelManager: Loading new model from " + path);
    auto model = std::make_shared<Model>(path);
    m_ModelCache[path] = model;
    return model;
}

} // namespace Luminumbra::Rendering