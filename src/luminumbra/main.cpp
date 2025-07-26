// src/luminumbra/main.cpp
#include "luminumbra/core/Engine.h"
#include <iostream>

int main() {
    try {
        Luminumbra::Core::Engine engine(1280, 720, "Luminumbra");
        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "An unrecoverable error occurred: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}