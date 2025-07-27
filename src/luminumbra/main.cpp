// src/luminumbra/main.cpp
#include "luminumbra/core/Engine.h"
#include "luminumbra/core/Debug.h"
#include "luminumbra/core/ResourceManager.h"
#include <iostream>

int main(int argc, char* argv[]) {
    Luminumbra::Core::ResourceManager::init(argv[0]);
    LOG("Starting Luminumbra Engine...");
    try {
        Luminumbra::Core::Engine engine(1920, 1080, "Luminumbra");
        LOG("Luminumbra Engine initialized successfully with dimensions 1920x1080.");
        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "An unrecoverable error occurred: " << e.what() << std::endl;
        return -1;
    }

    LOG("Main function finished gracefully.");
    return 0;
}
