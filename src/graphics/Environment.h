#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "graphics/Renderer.h"

namespace ww {

struct StageDef;

// ---------------------------------------------------------------------------
// EnvironmentBaker — deterministic landscape composition for land terrain
// (Island/Jungle segments), replacing the old 32px "green tile + random tan
// square" filler with composed 64px chunks.
//
// Each chunk is baked ONCE into a pooled texture (LRU by last use):
//   1. organic ground materials (grass / dry grass / dirt / forest floor /
//      sand) blended by world-space coherent noise — no tile lattice edges;
//   2. farmland parcels (crop rows, plowed, stubble, pasture, orchards) with
//      hedgerows and post-and-rail fences on their boundaries;
//   3. a continuous road network (main dirt route + curved lanes, cross- and
//      T-junctions, driveways) derived from a jittered lattice, so roads are
//      gentle curves rather than grid lines;
//   4. composed set pieces: farmsteads (farmhouse + barn + shed + haystack
//      + yard), hamlets (clustered houses + well/pond), military depots
//      (warehouse + fuel tanks on a cleared pad), ponds, rock outcrops;
//   5. scattered trees/vegetation with biome-driven density, avoiding roads,
//      buildings and crops (orchards and hedgerows excepted).
//
// Everything is keyed by world position + the land segment's bounds, so a
// replay of the same stage sees exactly the same landscape, and the layout
// never depends on time or RNG state. Scenery uses the muted scenery
// palette: interactive targets stay the brightest things on screen.
// ---------------------------------------------------------------------------
class EnvironmentBaker {
public:
    static constexpr int kChunk = 64;    // chunk edge, logical px
    static constexpr int kPoolSize = 224;  // resident chunk textures

    bool init(Renderer& r);
    void clear();
    // Reset the per-frame bake budget (call once per rendered frame).
    void beginFrame() { frameBakes_ = 0; }

    // Texture id of the composed chunk covering world position (cx, cy), or
    // -1 when (cx, cy) is not on composed land / baking is unavailable.
    int get(Renderer& r, const StageDef& stage, int cx, int cy);

    bool ready() const { return ready_; }

private:
    struct Entry {
        int slot = -1;
        float lo = -1.0f, hi = -1.0f;
        std::uint64_t lastUse = 0;
    };
    static std::uint64_t makeKey(int cx, int cy);

    int bake(Renderer& r, const StageDef& stage, int cx, int cy, float lo, float hi,
             int slot);

    Renderer* renderer_ = nullptr;
    std::vector<int> slotTex_;
    int slotNext_ = 0;
    std::unordered_map<std::uint64_t, Entry> cache_;
    std::uint64_t useClock_ = 0;
    int frameBakes_ = 0;
    static constexpr int kBakesPerFrame = 4;
    bool ready_ = false;
};

}  // namespace ww
