#pragma once

#include "graphics/Renderer.h"

namespace ww {

// Bakes the 5x7 font into a one-row glyph atlas and registers it with the
// renderer.
void buildFontAtlas(Renderer& renderer);

}  // namespace ww
