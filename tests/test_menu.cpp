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

// Moving the menu highlight emits a short "menu_select" click-beep on the
// title and options screens: exactly one event per actual movement, none for
// cancelled inputs (up+down in the same frame).
void test_menu_move_sfx() {
    GameData data;
    Game game(data);

    // Drain pending events, counting menu click-beep sounds.
    int clicks = 0;
    auto drain = [&game, &clicks] {
        GameEvent ev;
        while (game.popEvent(ev))
            if (ev.type == GameEvent::Type::Sound && ev.name == "menu_select")
                ++clicks;
    };

    // Title: one beep per highlight movement.
    tap(game, setDown);
    drain();
    CHECK(clicks == 1);

    // Opposing inputs on the same frame cancel: no move, no beep.
    GameInput both{};
    both.up = both.down = true;
    game.submitInput(both);
    game.update(1.0f / 60.0f);
    game.submitInput(GameInput{});
    game.update(1.0f / 60.0f);
    drain();
    CHECK(game.menuSelection() == 1);
    CHECK(clicks == 1);

    // Walk to OPTIONS (index 3) and enter.
    tap(game, setDown);
    tap(game, setDown);
    drain();
    CHECK(clicks == 3);
    tap(game, setConfirm);
    CHECK(game.state() == GameState::Options);

    // Options: same behaviour when switching items.
    tap(game, setDown);
    drain();
    CHECK(clicks == 4);

    // Wrapping past the top also beeps exactly once.
    tap(game, setUp);  // 1 -> 0
    drain();           // clear the event from re-centering
    const int before = clicks;
    tap(game, setUp);  // wraps 0 -> 5
    CHECK(game.menuSelection() == 5);
    drain();
    CHECK(clicks == before + 1);
}

// Confirming a menu item AND leaving any menu screen (esc/back) emit the
// "menu_confirm" click: title entries, the options BACK row, leaving options
// from any row, and leaving high scores / help. A value row in options
// ignores confirm, so confirm there stays silent.
void test_menu_confirm_sfx() {
    GameData data;
    Game game(data);

    int confirms = 0;
    auto drain = [&game, &confirms] {
        GameEvent ev;
        while (game.popEvent(ev))
            if (ev.type == GameEvent::Type::Sound && ev.name == "menu_confirm")
                ++confirms;
    };

    // Title: confirming an entry selects it and clicks.
    tap(game, setDown);
    drain();  // ignore the move click
    tap(game, setConfirm);  // -> HighScores
    drain();
    CHECK(game.state() == GameState::HighScores);
    CHECK(confirms == 1);

    // Back/esc leaving HighScores clicks too.
    tap(game, setBack);
    drain();
    CHECK(game.state() == GameState::Title);
    CHECK(confirms == 2);

    // Entering options (a title selection) clicks.
    tap(game, setDown);
    tap(game, setDown);
    tap(game, setDown);
    tap(game, setConfirm);  // -> Options, selection resets to 0
    drain();
    CHECK(game.state() == GameState::Options);
    CHECK(confirms == 3);

    // Confirm on a value row (difficulty) is inert and silent.
    tap(game, setConfirm);
    drain();
    CHECK(game.state() == GameState::Options);
    CHECK(confirms == 3);

    // Back from a value row leaves options with a click.
    tap(game, setBack);
    drain();
    CHECK(game.state() == GameState::Title);
    CHECK(confirms == 4);

    // BACK row: confirm clicks and exits.
    tap(game, setDown);
    tap(game, setDown);
    tap(game, setDown);
    tap(game, setConfirm);  // -> Options
    for (int i = 0; i < 5; ++i) tap(game, setDown);  // walk to the BACK row
    drain();
    CHECK(confirms == 5);  // only the options entry clicked
    tap(game, setConfirm);
    drain();
    CHECK(game.state() == GameState::Title);
    CHECK(confirms == 6);

    // Help: entering and leaving both click.
    tap(game, setDown);
    tap(game, setDown);
    tap(game, setConfirm);  // index 2 -> Help
    drain();
    CHECK(game.state() == GameState::Help);
    CHECK(confirms == 7);
    tap(game, setBack);
    drain();
    CHECK(game.state() == GameState::Title);
    CHECK(confirms == 8);
}
