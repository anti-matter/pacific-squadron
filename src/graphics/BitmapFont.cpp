#include "graphics/BitmapFont.h"

#include <vector>

#include "graphics/Font5x7.h"

namespace ww {

void buildFontAtlas(Renderer& renderer) {
    constexpr int kCount = 95;  // ASCII 32..126
    std::vector<std::uint32_t> atlas(static_cast<std::size_t>(kCount * 6) * 8, 0);
    for (int g = 0; g < kCount; ++g) {
        for (int y = 0; y < kGlyphH; ++y) {
            for (int x = 0; x < kGlyphW; ++x) {
                if ((kFont5x7[g][y] >> (4 - x)) & 0x1)
                    atlas[static_cast<std::size_t>(y) * kCount * 6 +
                          static_cast<std::size_t>(g) * 6 + x] = 0xFFFFFFFFu;
            }
        }
    }
    const int id = renderer.createTextureFromPixels(
        "font5x7", reinterpret_cast<const std::uint8_t*>(atlas.data()),
        kCount * 6, 8);
    renderer.initFontAtlas(id);
}

}  // namespace ww
