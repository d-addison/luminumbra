#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>

void error_callback(int error, const char* description)
{
    fprintf(stderr, "Error: %s\n", description);
}

int main()
{
    std::cout << "Luminumbra Engine Initializing..." << std::endl;

    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwSetErrorCallback(error_callback);

    // Request OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Luminumbra", NULL, NULL);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    // Load OpenGL functions with GLAD
    if (!gladLoadGL(glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        glfwTerminate();
        return -1;
    }

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

    // Set the initial viewport size
    glViewport(0, 0, 1280, 720);

    // --- Main Game Loop ---
    while (!glfwWindowShouldClose(window))
    {
        // --- Input ---
        glfwPollEvents();

        // --- Rendering ---
        // Clear the screen to a color from the Umbra palette (#483D8B)
        glClearColor(0.28f, 0.24f, 0.55f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // --- Swap Buffers ---
        glfwSwapBuffers(window);
    }

    // --- Shutdown ---
    std::cout << "Shutting down." << std::endl;
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}