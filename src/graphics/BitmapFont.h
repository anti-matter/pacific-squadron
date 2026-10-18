#pragma once

#include "graphics/Renderer.h"

namespace ww {

// Bakes the arcade stroke font into a one-row glyph atlas rasterized at the
// given pixel scale (1.0 = 7x11 cells, 3.0 = 21x33 cells, ...). Returns the
// texture id. The overlay text path bakes one atlas per display scale so
// glyphs are pixel-exact at their final on-screen size.
int buildFontAtlas(Renderer& renderer, float px = 1.0f);

}  // namespace ww
