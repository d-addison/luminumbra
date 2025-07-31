add_executable(luminumbra_server
    # List all .cpp files here
    main_server.cpp
    HostManager.cpp
    # ... etc
)
target_link_libraries(luminumbra_server PRIVATE luminumbra_common)
