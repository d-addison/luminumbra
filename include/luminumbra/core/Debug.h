// include/luminumbra/core/Debug.h
#pragma once

#include <iostream>
#include <string> // Add this

// Define a simple logging macro
#ifdef NDEBUG
    #define LOG(message) ((void)0)
#else
    // Using a template allows it to accept various types that can be streamed to std::cout
    template<typename T>
    void LogMessage(const T& message) {
        std::cout << "[LOG] " << message << std::endl;
    }
    #define LOG(message) LogMessage(message)
#endif