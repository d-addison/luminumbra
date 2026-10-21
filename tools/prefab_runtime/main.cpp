#include "authoring/PrefabRuntime.h"

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        std::string project, generation, manifest, instance;
        glm::dmat4 placement(1.0);
        bool have_placement = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--help" && argc == 2) {
                std::cout
                    << "luminumbra_prefab_inspect --project DIR --generation ID "
                       "--manifest-sha256 HASH --instance ID [--placement '[16 column-major "
                       "numbers]']\n"
                       "Loads a pinned compiled generation and instantiates its real ECS "
                       "hierarchy.\n"
                       "Headless inspection only; no rendering, physics, scripts or save writes.\n";
                return 0;
            }
            if (++i == argc)
                throw std::runtime_error("Missing option value");
            const std::string value(argv[i]);
            if (option == "--placement") {
                if (have_placement)
                    throw std::runtime_error("Duplicate placement option");
                const auto matrix = nlohmann::json::parse(value);
                if (!matrix.is_array() || matrix.size() != 16)
                    throw std::runtime_error("Placement requires 16 column-major numbers");
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) {
                        if (!matrix[c * 4 + r].is_number())
                            throw std::runtime_error("Placement must be numeric");
                        placement[c][r] = matrix[c * 4 + r].get<double>();
                    }
                have_placement = true;
                continue;
            }
            auto* target = option == "--project"           ? &project
                           : option == "--generation"      ? &generation
                           : option == "--manifest-sha256" ? &manifest
                           : option == "--instance"        ? &instance
                                                           : nullptr;
            if (!target || !target->empty() || value.empty())
                throw std::runtime_error("Unknown, duplicate or empty option: " + option);
            *target = value;
        }
        if (project.empty() || generation.empty() || manifest.empty() || instance.empty())
            throw std::runtime_error("Required option missing; use --help");
        const auto asset = Luminumbra::Authoring::PrefabAsset::Load(project, generation, manifest);
        entt::registry registry;
        Luminumbra::Authoring::PrefabScene scene(registry);
        scene.Replace(instance, asset, placement);
        std::cout << scene.Inspect(instance).dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Prefab refused: " << error.what() << '\n';
        return 1;
    }
}
