// Main-menu navigation regression tests: 4 items with wrap-around, and the
// QUIT entry emitting a Quit event for the application layer.
#include "test_framework.h"

#include "game/Game.h"
#include "game/GameData.h"

using namespace ww;

static void tap(Game& game, void (*setter)(GameInput&, bool)) {
    GameInput on{};
    setter(on, true);
    game.submitInput(on);
    game.update(1.0f / 60.0f);
    GameInput off{};
    setter(off, false);
    game.submitInput(off);
    game.update(1.0f / 60.0f);
}

static void setDown(GameInput& g, bool v) { g.down = v; }
static void setUp(GameInput& g, bool v) { g.up = v; }
static void setConfirm(GameInput& g, bool v) { g.confirm = v; }
static void setBack(GameInput& g, bool v) { g.back = v; }

void test_menu_quit() {
    GameData data;
    Game game(data);
    CHECK(game.state() == GameState::Title);
    CHECK(game.menuSelection() == 0);

    // Four downs land on QUIT (index 4 of 5).
    for (int i = 0; i < 4; ++i) tap(game, setDown);
    CHECK(game.menuSelection() == 4);

    // Wrapping: one more down cycles back to START MISSION, and one up
    // from there goes back to QUIT.
    tap(game, setDown);
    CHECK(game.menuSelection() == 0);
    tap(game, setUp);
    CHECK(game.menuSelection() == 4);

    // Confirming QUIT emits a Quit event and stays on the title
    // (the application layer performs the actual shutdown).
    tap(game, setConfirm);
    bool sawQuit = false;
    GameEvent ev;
    while (game.popEvent(ev))
        if (ev.type == GameEvent::Type::Quit) sawQuit = true;
    CHECK(sawQuit);
    CHECK(game.state() == GameState::Title);

    // The other entries still work: index 3 opens Options, index 2 Help.
    tap(game, setUp);
    CHECK(game.menuSelection() == 3);
    tap(game, setConfirm);
    CHECK(game.state() == GameState::Options);
    tap(game, setBack);
    CHECK(game.state() == GameState::Title);
    CHECK(game.menuSelection() == 0);
    tap(game, setDown);
    tap(game, setDown);
    CHECK(game.menuSelection() == 2);
    tap(game, setConfirm);
    CHECK(game.state() == GameState::Help);
    tap(game, setBack);
    CHECK(game.state() == GameState::Title);
}
