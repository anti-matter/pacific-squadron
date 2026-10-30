#include "test_framework.h"

#include "game/Coastline.h"

using namespace ww;

// seaSurfaceAt() must reject only the narrow fringe of ocean rows beside a
// shore band's sea-facing edge (where the baked waterline can bulge
// seaward), never the ocean as a whole. An earlier version tested the
// margin without side bounds ("worldY < edge + margin" is trivially true
// for every row on the far side), so entire oceans read as invalid sea:
// the ship sea-hold could never latch, the rescue walk found nothing, and
// ships sailed landward unheld onto the terrain (stage 5 report). This
// pins the stage-5-style sandwich: ocean, shore, ocean, shore, jungle.
void test_sea_surface_fringe() {
    StageDef stage;
    stage.name = "coastline fringe";
    stage.scrollSpeed = 62.0f;
    stage.duration = 100.0f;
    stage.terrain = {
        {0.0f, 1000.0f, TerrainKind::Ocean},
        {1000.0f, 1100.0f, TerrainKind::Shore},
        {1100.0f, 3000.0f, TerrainKind::Ocean},  // sandwiched ocean
        {3000.0f, 3100.0f, TerrainKind::Shore},
        {3100.0f, 9000.0f, TerrainKind::Jungle},
    };
    const float m = coast::kShipMargin;

    // Mid-ocean rows are open water everywhere, far from any fringe.
    CHECK(seaSurfaceAt(stage, 500.0f));    // ocean below the first band
    CHECK(seaSurfaceAt(stage, 300.0f));    // far below: not the second
                                           // band's fringe either
    CHECK(seaSurfaceAt(stage, 1500.0f));   // middle of the sandwich
    CHECK(seaSurfaceAt(stage, 2900.0f));   // below the upper fringe

    // Fringe: only within one margin of each sea-facing edge.
    CHECK(!seaSurfaceAt(stage, 1000.0f - m * 0.5f));  // under lower edge
    CHECK(seaSurfaceAt(stage, 1000.0f - m * 2.0f));
    CHECK(!seaSurfaceAt(stage, 1100.0f + m * 0.5f));  // past upper edge
    CHECK(seaSurfaceAt(stage, 1100.0f + m * 2.0f));

    // Land is never open sea.
    CHECK(!seaSurfaceAt(stage, 1050.0f));  // shore band
    CHECK(!seaSurfaceAt(stage, 5000.0f));  // jungle
}
