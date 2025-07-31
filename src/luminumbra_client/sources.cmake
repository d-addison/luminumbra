# This file lists all source files for the 'luminumbra_client' target.
set(CLIENT_SOURCES
    main_client.cpp

    # Audio
    audio/AudioManagerFactory.cpp
    audio/MiniaudioManager.cpp

    # UI
    ui/Rml_UIManager.cpp

    # Rendering (even if empty, list it)
    rendering/RenderSystem.cpp
)