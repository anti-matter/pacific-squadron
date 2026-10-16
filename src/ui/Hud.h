#pragma once

#include "game/Game.h"
#include "graphics/Renderer.h"

namespace ww {

// Heads-up display in logical-resolution coordinates.
class Hud {
public:
    void draw(Renderer& r, const Game& game) const;
    void drawDebug(Renderer& r, const Game& game, float fps) const;
};

}  // namespace ww
