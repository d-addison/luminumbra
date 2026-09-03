#pragma once

enum class GameState {
    SPLASH,
    MAIN_MENU,
    WORLD_LOADING,
    SETTINGS,
    WORLD_GEN_VIEWER,
    IN_GAME,
    EXITING
};

class GameStateManager {
public:
    GameStateManager()
        : currentState(GameState::SPLASH) {}

    GameState GetCurrentState() const {
        return currentState;
    }

    void SetState(GameState newState) {
        currentState = newState;
    }

private:
    GameState currentState;
};
