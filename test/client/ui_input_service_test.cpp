#include "app/UiInputRouter.h"
#include "app/UiInputService.h"
#include "debug/DeveloperConsole.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace Luminumbra::Client::App {
namespace {
constexpr InputContextPolicy kGameplay{InputOwner::Gameplay, CursorMode::Captured, false};
constexpr InputContextPolicy kMenu{InputOwner::RmlUi, CursorMode::Visible, false};
constexpr InputContextPolicy kText{InputOwner::Developer, CursorMode::Visible, true};

TEST(UiInputService, EmptyAndUnknownContextsRefuseWithoutChangingState) {
    UiInputService service;
    const InputContextId unknown{42};
    EXPECT_EQ(service.Focused(), InputContextId{});
    EXPECT_EQ(service.Owner(), InputOwner::Unfocused);
    EXPECT_EQ(service.Cursor(), CursorMode::Visible);
    EXPECT_FALSE(service.TextEntry());
    EXPECT_FALSE(service.GameplayKeyAllowed(87));
    EXPECT_FALSE(service.Back());
    for (const auto id : {InputContextId{}, unknown}) {
        EXPECT_FALSE(service.Focus(id));
        EXPECT_FALSE(service.Unfocus(id));
        EXPECT_FALSE(service.Update(id, kText));
        EXPECT_FALSE(service.Unregister(id));
    }
    EXPECT_EQ(service.RegisteredCount(), 0u);
    EXPECT_EQ(service.Revision(), 0u);
    EXPECT_TRUE(service.TakeReleasedKeys().none());
}

TEST(UiInputService, CapacityRefusalAndStaleIdsLeaveFocusAndPolicyUnchanged) {
    UiInputService service;
    std::array<InputContextId, UiInputService::kMaxContexts> ids{};
    for (auto& id : ids) {
        id = service.Register(kGameplay);
        ASSERT_NE(id, InputContextId{});
        ASSERT_TRUE(service.Focus(id));
    }
    const auto revision = service.Revision();
    EXPECT_EQ(service.Register(kText), InputContextId{});
    EXPECT_EQ(service.RegisteredCount(), ids.size());
    EXPECT_EQ(service.Focused(), ids.back());
    EXPECT_EQ(service.Owner(), InputOwner::Gameplay);
    EXPECT_EQ(service.Revision(), revision);
    const auto stale = ids.front();
    ASSERT_TRUE(service.Unregister(stale));
    const auto replacement = service.Register(kText);
    ASSERT_NE(replacement, InputContextId{});
    EXPECT_NE(replacement, stale);
    EXPECT_FALSE(service.Focus(stale));
    EXPECT_FALSE(service.Update(stale, kText));
    EXPECT_FALSE(service.Unregister(stale));
    EXPECT_EQ(service.Revision(), revision);
    EXPECT_TRUE(service.Focus(replacement));
    EXPECT_EQ(service.Owner(), InputOwner::Developer);
    EXPECT_TRUE(service.Back());
    EXPECT_EQ(service.Focused(), ids.back());
    {
        ScopedInputContext refused(service, kMenu);
        EXPECT_EQ(refused.Id(), InputContextId{});
    }
    EXPECT_EQ(service.RegisteredCount(), ids.size());
}

TEST(UiInputService, BackRestoresOriginAndRepeatedFocusNeverDuplicatesEntries) {
    UiInputService service;
    ScopedInputContext root(service, kGameplay);
    ScopedInputContext menu(service, kMenu);
    ScopedInputContext text(service, kText);
    ASSERT_TRUE(service.Focus(root.Id()));
    ASSERT_TRUE(service.Focus(menu.Id()));
    ASSERT_TRUE(service.Focus(text.Id()));
    const auto revision = service.Revision();
    EXPECT_TRUE(service.Focus(text.Id()));
    EXPECT_EQ(service.Revision(), revision);
    ASSERT_TRUE(service.Back());
    EXPECT_EQ(service.Focused(), menu.Id());
    EXPECT_EQ(service.Cursor(), CursorMode::Visible);
    ASSERT_TRUE(service.Back());
    EXPECT_EQ(service.Focused(), root.Id());
    EXPECT_EQ(service.Cursor(), CursorMode::Captured);
    EXPECT_FALSE(service.Back());
    ASSERT_TRUE(service.Focus(menu.Id()));
    ASSERT_TRUE(service.Focus(text.Id()));
    ASSERT_TRUE(service.Focus(menu.Id()));
    ASSERT_TRUE(service.Back());
    EXPECT_EQ(service.Focused(), text.Id());
    ASSERT_TRUE(service.Back());
    EXPECT_EQ(service.Focused(), root.Id());
    EXPECT_FALSE(service.Back());
}

TEST(UiInputService, ScopedTeardownRemovesFocusedCoveredAndInactiveRegistrations) {
    UiInputService service;
    {
        ScopedInputContext root(service, kGameplay);
        ASSERT_TRUE(service.Focus(root.Id()));
        InputContextId covered;
        {
            ScopedInputContext menu(service, kMenu);
            covered = menu.Id();
            ASSERT_TRUE(service.Focus(covered));
            const auto top = service.Register(kText);
            ASSERT_TRUE(service.Focus(top));
        }
        EXPECT_EQ(service.RegisteredCount(), 2u);
        EXPECT_FALSE(service.Focus(covered));
        ASSERT_TRUE(service.Back());
        EXPECT_EQ(service.Focused(), root.Id());
        {
            ScopedInputContext focused(service, kMenu);
            ASSERT_TRUE(service.Focus(focused.Id()));
        }
        EXPECT_EQ(service.Focused(), root.Id());
        {
            ScopedInputContext inactive(service, kText);
            EXPECT_FALSE(service.Unfocus(inactive.Id()));
        }
    }
    EXPECT_EQ(service.RegisteredCount(), 1u);
    EXPECT_EQ(service.Focused(), InputContextId{});
    EXPECT_EQ(service.Owner(), InputOwner::Unfocused);
    EXPECT_EQ(service.Cursor(), CursorMode::Visible);
}

TEST(UiInputService, UnfocusRemovesCoveredEntriesAndCanReleaseTheRoot) {
    UiInputService service;
    ScopedInputContext root(service, kGameplay);
    ScopedInputContext menu(service, kMenu);
    ScopedInputContext text(service, kText);
    ASSERT_TRUE(service.Focus(root.Id()));
    ASSERT_TRUE(service.Focus(menu.Id()));
    ASSERT_TRUE(service.Focus(text.Id()));
    const auto revision = service.Revision();
    ASSERT_TRUE(service.Unfocus(menu.Id()));
    EXPECT_EQ(service.Revision(), revision);
    EXPECT_FALSE(service.Unfocus(menu.Id()));
    ASSERT_TRUE(service.Unfocus(text.Id()));
    EXPECT_EQ(service.Focused(), root.Id());
    ASSERT_TRUE(service.Unfocus(root.Id()));
    EXPECT_EQ(service.Owner(), InputOwner::Unfocused);
    EXPECT_TRUE(service.Focus(root.Id()));
    EXPECT_EQ(service.Owner(), InputOwner::Gameplay);
}

TEST(UiInputService, CaptureChangeReleasesHeldKeysUntilPhysicalRelease) {
    UiInputService service;
    ScopedInputContext root(service, kGameplay);
    ScopedInputContext overlay(service, kGameplay);
    ASSERT_TRUE(service.Focus(root.Id()));
    service.KeyDown(0);
    service.KeyDown(87);
    service.KeyDown(511);
    ASSERT_TRUE(service.GameplayKeyAllowed(87));
    ASSERT_TRUE(service.Focus(overlay.Id()));
    auto released = service.TakeReleasedKeys();
    EXPECT_EQ(released.count(), 3u);
    EXPECT_TRUE(released.test(0));
    EXPECT_TRUE(released.test(87));
    EXPECT_TRUE(released.test(511));
    EXPECT_TRUE(service.TakeReleasedKeys().none());
    EXPECT_FALSE(service.GameplayKeyAllowed(87));
    ASSERT_TRUE(service.Back());
    service.KeyDown(87);
    EXPECT_FALSE(service.GameplayKeyAllowed(87));
    service.KeyUp(87);
    EXPECT_TRUE(service.GameplayKeyAllowed(87));
    service.KeyDown(87);
    EXPECT_TRUE(service.GameplayKeyAllowed(87));
    service.KeyUp(0);
    service.KeyUp(511);
    service.KeyUp(87);
    service.TakeReleasedKeys();
    service.ReleaseHeldInput();
    EXPECT_TRUE(service.TakeReleasedKeys().none());
    EXPECT_TRUE(service.GameplayKeyAllowed(0));
    EXPECT_TRUE(service.GameplayKeyAllowed(511));
}

TEST(UiInputService, InvalidKeyCodesCannotAllocateOrEnableGameplay) {
    UiInputService service;
    ScopedInputContext root(service, kGameplay);
    ASSERT_TRUE(service.Focus(root.Id()));
    for (const int key :
         {-1, 512, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        service.KeyDown(key);
        EXPECT_TRUE(service.KeyQuarantined(key));
        EXPECT_FALSE(service.GameplayKeyAllowed(key));
        service.KeyUp(key);
    }
    service.ReleaseHeldInput();
    EXPECT_TRUE(service.TakeReleasedKeys().none());
}

TEST(UiInputService, PolicyChangesOnlyReleaseTheActiveContextAndPendingReleasesSurviveKeyUp) {
    UiInputService service;
    ScopedInputContext root(service, kGameplay);
    ScopedInputContext menu(service, kMenu);
    ASSERT_TRUE(service.Focus(root.Id()));
    service.KeyDown(87);
    const auto revision = service.Revision();
    EXPECT_TRUE(service.Update(root.Id(), kGameplay));
    EXPECT_TRUE(service.Update(menu.Id(), kText));
    EXPECT_EQ(service.Revision(), revision);
    EXPECT_TRUE(service.GameplayKeyAllowed(87));
    ASSERT_TRUE(service.Update(root.Id(), kText));
    EXPECT_GT(service.Revision(), revision);
    EXPECT_EQ(service.Owner(), InputOwner::Developer);
    EXPECT_TRUE(service.TextEntry());
    EXPECT_EQ(service.Cursor(), CursorMode::Visible);
    service.KeyUp(87);
    EXPECT_TRUE(service.TakeReleasedKeys().test(87));
    ASSERT_TRUE(service.Update(root.Id(), kGameplay));
    EXPECT_TRUE(service.GameplayKeyAllowed(87));
    ASSERT_TRUE(service.Update(root.Id(), {InputOwner::Gameplay, CursorMode::Visible, true}));
    EXPECT_EQ(service.Owner(), InputOwner::Developer);
    EXPECT_FALSE(service.GameplayKeyAllowed(87));
}

TEST(UiInputService, WindowFocusLossPreservesNavigationAndRequiresKeyReleaseAfterReturn) {
    UiInputService service;
    ScopedInputContext root(service, kGameplay);
    ASSERT_TRUE(service.Focus(root.Id()));
    service.KeyDown(87);
    service.SetWindowFocused(false);
    EXPECT_EQ(service.Owner(), InputOwner::Unfocused);
    EXPECT_EQ(service.Cursor(), CursorMode::Visible);
    EXPECT_FALSE(service.TextEntry());
    EXPECT_FALSE(service.GameplayKeyAllowed(87));
    EXPECT_TRUE(service.TakeReleasedKeys().test(87));
    const auto revision = service.Revision();
    service.SetWindowFocused(false);
    EXPECT_EQ(service.Revision(), revision);
    service.KeyDown(65);
    service.SetWindowFocused(true);
    EXPECT_EQ(service.Focused(), root.Id());
    EXPECT_EQ(service.Cursor(), CursorMode::Captured);
    EXPECT_FALSE(service.GameplayKeyAllowed(87));
    EXPECT_FALSE(service.GameplayKeyAllowed(65));
    service.KeyUp(87);
    service.KeyUp(65);
    EXPECT_TRUE(service.GameplayKeyAllowed(87));
    EXPECT_TRUE(service.GameplayKeyAllowed(65));
}

TEST(UiInputService, FocusedTeardownReleasesKeysAndDrainsAllRegistrations) {
    UiInputService service;
    {
        ScopedInputContext root(service, kGameplay);
        ASSERT_TRUE(service.Focus(root.Id()));
        {
            ScopedInputContext text(service, kText);
            ASSERT_TRUE(service.Focus(text.Id()));
            service.KeyDown(65);
        }
        EXPECT_EQ(service.Focused(), root.Id());
        EXPECT_TRUE(service.TakeReleasedKeys().test(65));
        EXPECT_FALSE(service.GameplayKeyAllowed(65));
    }
    EXPECT_EQ(service.RegisteredCount(), 0u);
    EXPECT_EQ(service.Owner(), InputOwner::Unfocused);
}

TEST(UiInputIntegration, RegisteredTextEntryBlocksGameplayAndDeveloperShortcuts) {
    for (const auto owner : {InputOwner::Developer, InputOwner::RmlUi}) {
        UiInputRouter router;
        EXPECT_FALSE(router.SetOwnership({}));
        UiInputOwnership world;
        world.in_world = true;
        router.SetOwnership(world);
        ScopedInputContext entry(router.Contexts(), {owner, CursorMode::Visible, true});
        ASSERT_TRUE(router.Contexts().Focus(entry.Id()));
        EXPECT_TRUE(router.SetOwnership(world));
        EXPECT_FALSE(router.SetOwnership(world));
        EXPECT_FALSE(router.GameplayAllowed());
        EXPECT_FALSE(router.GameplayMouseAllowed());
        EXPECT_FALSE(router.CursorCaptured());
        EXPECT_FALSE(router.GameplayKeyAllowed(87));
        EXPECT_EQ(router.Key(87, 1, false, true),
                  owner == InputOwner::RmlUi ? KeyRoute::RmlUi : KeyRoute::Ignore);
        EXPECT_EQ(router.Key(294, 1, false, true),
                  owner == InputOwner::RmlUi ? KeyRoute::RmlUi : KeyRoute::Ignore);
        EXPECT_EQ(router.Character('w'), owner);
        EXPECT_TRUE(router.Contexts().Back());
        EXPECT_TRUE(router.SetOwnership(world));
        EXPECT_TRUE(router.GameplayAllowed());
        EXPECT_FALSE(router.GameplayKeyAllowed(87));
        EXPECT_EQ(router.Key(87, 2, false, false), KeyRoute::Ignore);
        router.Key(87, 0, false, false);
        EXPECT_EQ(router.Key(87, 1, false, false), KeyRoute::Gameplay);
    }
}

TEST(UiInputIntegration, ConsoleBackRestoresOriginAndReopeningKeepsRegistrationBounded) {
    Debug::DeveloperConsole console;
    console.Initialize({});
    UiInputRouter router;
    UiInputOwnership world;
    world.in_world = true;
    router.SetOwnership(world);
    ScopedInputContext origin(router.Contexts(), kMenu);
    ASSERT_TRUE(router.Contexts().Focus(origin.Id()));
    for (int iteration = 0; iteration < 3; ++iteration) {
        console.SetOpen(true);
        world.console_open = console.IsOpen();
        ASSERT_TRUE(router.SetOwnership(world));
        EXPECT_EQ(router.Owner(), InputOwner::Developer);
        EXPECT_NE(router.Contexts().Focused(), origin.Id());
        EXPECT_EQ(router.Key(256, 1, false, false), KeyRoute::DeveloperEscape);
        ASSERT_TRUE(console.CloseTopInteraction());
        world.console_open = console.IsOpen();
        ASSERT_TRUE(router.SetOwnership(world));
        EXPECT_EQ(router.Contexts().Focused(), origin.Id());
        EXPECT_EQ(router.Owner(), InputOwner::RmlUi);
        EXPECT_FALSE(router.CursorCaptured());
        EXPECT_EQ(router.Contexts().RegisteredCount(), 3u);
    }
    ASSERT_TRUE(router.Contexts().Back());
    router.SetOwnership(world);
    EXPECT_EQ(router.Owner(), InputOwner::Gameplay);
    EXPECT_TRUE(router.CursorCaptured());
    console.Shutdown();
}

TEST(UiInputIntegration, ConsoleRegistrationRemainsAvailableAtCapacity) {
    UiInputRouter router;
    UiInputOwnership world;
    world.in_world = true;
    router.SetOwnership(world);
    for (std::size_t index = 2; index < UiInputService::kMaxContexts; ++index) {
        ASSERT_NE(router.Contexts().Register(kMenu), InputContextId{});
    }
    EXPECT_EQ(router.Contexts().Register(kMenu), InputContextId{});
    world.console_open = true;
    ASSERT_TRUE(router.SetOwnership(world));
    EXPECT_EQ(router.Owner(), InputOwner::Developer);
    EXPECT_FALSE(router.CursorCaptured());
    world.console_open = false;
    ASSERT_TRUE(router.SetOwnership(world));
    EXPECT_EQ(router.Owner(), InputOwner::Gameplay);
    EXPECT_TRUE(router.CursorCaptured());
}
} // namespace
} // namespace Luminumbra::Client::App
