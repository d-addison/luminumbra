# Luminumbra Enhanced UI System - Usage Guide

## Overview

The new UI system provides a modern, component-based architecture with data binding, theming, and hot-reload capabilities built on top of RmlUi.

## Quick Start

### 1. Initialize the System

```cpp
#include "ui/UIIntegration.h"

// In your initialization code:
UIIntegration::InitializeNewUISystem("assets", window, audioManager);
UIIntegration::EnableDevelopmentMode(true); // Enable hot-reload and debugger
```

### 2. Basic Usage

```cpp
// Access the UI state manager
auto& uiState = GetUIState();

// Update player health
uiState.SetPlayerHealth(75, 100);

// Show a notification
uiState.ShowNotification("Game saved successfully!");

// Navigate to different screens
uiState.NavigateToDocument("world_selection.rml");
```

### 3. Creating Components

```cpp
// Get the UI manager
auto& uiManager = GetUIManager();

// Create a button component
auto button = uiManager.CreateButton("my_button");
button->SetText("Click Me!");
button->SetStyle(Button::Style::Primary);
button->SetClickHandler([]() {
    GetUIState().ShowNotification("Button clicked!");
});

// Create an input component with validation
auto input = uiManager.CreateInput("username_input");
input->SetPlaceholder("Enter username...");
input->SetRequired(true);
input->SetValidationPattern("[a-zA-Z0-9_]{3,16}");
input->SetValidationMessage("Username must be 3-16 characters, letters, numbers, and underscores only");
```

### 4. Data Binding

```cpp
// Bind UI components to state properties
auto nameInput = uiManager.CreateInput("player_name");
nameInput->BindValue(uiState.playerName);

// Bind button visibility to game state
auto pauseButton = uiManager.CreateButton("pause_btn");
pauseButton->BindVisibility(uiState.isInGame, [](bool inGame) { return inGame; });

// Bind world list to dynamic data
auto worldList = uiManager.CreateComponent<WorldList>("world_list");
worldList->BindWorlds(uiState.worldList);
worldList->SetSelectionCallback([](const WorldInfo& world) {
    GetUIState().selectedWorldId.Set(world.id);
});
```

## Component System

### Button Component

```cpp
auto button = uiManager.CreateButton("my_button");
button->SetText("Save Game");
button->SetStyle(Button::Style::Success);
button->SetSize(Button::Size::Large);
button->SetIcon("icons/save.png");

button->SetClickHandler([]() {
    // Handle click
});

// Data binding
button->BindEnabled(gameState.canSave);
button->BindText(saveStatus.message);
```

### Input Component

```cpp
auto input = uiManager.CreateInput("world_name");
input->SetType(Input::Type::Text);
input->SetPlaceholder("World name...");
input->SetRequired(true);
input->SetMaxLength(32);

// Custom validation
input->SetCustomValidator([](const std::string& value) {
    return !value.empty() && value.find_first_of("/\\:*?\"<>|") == std::string::npos;
});

input->SetChangeHandler([](const std::string& value) {
    // Handle value change
});
```

### Panel Component

```cpp
auto panel = uiManager.CreatePanel("settings_panel");
panel->SetTitle("Graphics Settings");
panel->SetStyle(Panel::Style::Elevated);
panel->SetCollapsible(true);

panel->SetContent(R"(
    <div class="form-group">
        <label>Resolution</label>
        <select id="resolution_select">
            <option>1920x1080</option>
            <option>2560x1440</option>
        </select>
    </div>
)");
```

## Theming System

### Using CSS Variables

```css
:root {
    --color-primary: #4f5b66;
    --color-accent: #d80032;
    --spacing-md: 16px;
    --border-radius: 8px;
}

.my-button {
    background-color: var(--color-primary);
    padding: var(--spacing-md);
    border-radius: var(--border-radius);
}
```

### Component Classes

```css
/* Use predefined component classes */
.btn-primary { /* Primary button style */ }
.btn-success { /* Success button style */ }
.form-control { /* Input field style */ }
.panel-elevated { /* Elevated panel style */ }
```

## State Management

### Reactive Properties

```cpp
// Create observable properties
Property<std::string> playerName{"Player"};
Property<int> playerLevel{1};
Property<std::vector<Item>> inventory;

// Subscribe to changes
playerLevel.Subscribe([](const int& oldLevel, const int& newLevel) {
    if (newLevel > oldLevel) {
        GetUIState().ShowNotification("Level up! You are now level " + std::to_string(newLevel));
    }
});

// Modify values (triggers subscribers)
playerLevel.Set(5);
inventory.Add(newItem);
inventory.Clear();
```

### Global State Access

```cpp
// Access the global UI state
auto& state = GetUIState();

// Player properties
state.playerName.Set("Hero");
state.SetPlayerHealth(80, 100);

// Game state
state.currentGameState.Set(GameState::InGame);
state.isInGame.Set(true);

// World management
state.worldList.Add(newWorld);
state.selectedWorldId.Set("world_123");

// UI state
state.ShowNotification("Achievement unlocked!", 3.0f);
state.NavigateToDocument("inventory.rml");
```

## Development Features

### Hot-Reload

```cpp
// Enable hot-reload (automatically watches files)
EnableUIHotReload(true);

// Manual reload
ReloadUITheme();
```

When hot-reload is enabled:
- CSS files (`.rcss`) trigger theme reloads
- Layout files (`.rml`) trigger document reloads
- Changes are applied automatically during development

### Debug Tools

```cpp
// Show/hide the RmlUi debugger
ShowUIDebugger(true); // F8 key also toggles

// Enable development mode (hot-reload + debugger)
UIIntegration::EnableDevelopmentMode(true);
```

### Logging

The system provides detailed logging:
- `[UI]` - General UI operations
- `[UI HotReload]` - File watching and reload events
- `[UI Integration]` - System initialization and integration

## Migration from Old System

### Before (Old Rml_UIManager)

```cpp
Rml_UIManager uiManager("assets");
uiManager.Init(window, audioManager);
uiManager.RequestLoadDocument("main_menu.rml");

// Manual event binding
if (auto* button = document->GetElementById("play_btn")) {
    button->AddEventListener("click", new CustomEventListener());
}
```

### After (New UIManager)

```cpp
UIIntegration::InitializeNewUISystem("assets", window, audioManager);

// Component-based approach
auto button = GetUIManager().CreateButton("play_btn");
button->SetClickHandler([]() {
    GetUIState().NavigateToDocument("game.rml");
});

// Automatic data binding
button->BindEnabled(gameState.canPlay);
```

## Best Practices

1. **Use Data Binding**: Prefer reactive properties over manual updates
2. **Component Reuse**: Create reusable components for common UI patterns
3. **Consistent Theming**: Use CSS variables for consistent styling
4. **Error Handling**: Always validate user inputs with appropriate feedback
5. **Accessibility**: Use semantic HTML and proper focus management
6. **Performance**: Use visibility binding to hide/show expensive components

## Example: Complete World Selection Screen

```cpp
void SetupWorldSelectionScreen() {
    auto& uiManager = GetUIManager();
    auto& state = GetUIState();
    
    // Search input
    auto searchInput = uiManager.CreateInput("world_search");
    searchInput->SetPlaceholder("Search worlds...");
    searchInput->BindValue(state.worldSearchFilter);
    
    // World list
    auto worldList = uiManager.CreateComponent<WorldList>("world_list");
    worldList->BindWorlds(state.worldList);
    worldList->BindSelectedWorldId(state.selectedWorldId);
    worldList->BindSearchFilter(state.worldSearchFilter);
    
    worldList->SetSelectionCallback([](const WorldInfo& world) {
        // Enable load button when world is selected
        GetUIState().canLoadWorld.Set(true);
    });
    
    // Load button
    auto loadButton = uiManager.CreateButton("load_world_btn");
    loadButton->SetText("Load World");
    loadButton->SetStyle(Button::Style::Primary);
    loadButton->BindEnabled(state.canLoadWorld);
    
    loadButton->SetClickHandler([]() {
        auto selectedWorld = GetUIState().selectedWorldId.Get();
        if (!selectedWorld.empty()) {
            // Load the selected world
            LoadWorld(selectedWorld);
        }
    });
}
```

This new system provides a much more maintainable, scalable, and developer-friendly approach to UI development in Luminumbra.