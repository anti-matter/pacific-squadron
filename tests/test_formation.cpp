#include "test_framework.h"

#include "game/Formation.h"

using namespace ww;

void test_formation() {
    // Single: exactly one member at the anchor.
    auto one = formationOffsets(FormationShape::Single, 1);
    CHECK(one.size() == 1);
    CHECK_NEAR(one[0].x, 0.0f, 1e-5);
    CHECK_NEAR(one[0].y, 0.0f, 1e-5);

    // Horizontal line: symmetric about the anchor, correct count.
    auto line = formationOffsets(FormationShape::HLine, 5, 40.0f);
    CHECK(line.size() == 5);
    CHECK_NEAR(line[2].x, 0.0f, 1e-4);
    CHECK_NEAR(line[0].x, -80.0f, 1e-4);
    CHECK_NEAR(line[4].x, 80.0f, 1e-4);
    for (const auto& p : line) CHECK_NEAR(p.y, 0.0f, 1e-4);

    // V formation: leader at the tip, pairs behind and to each side.
    auto v = formationOffsets(FormationShape::VFormation, 5, 36.0f);
    CHECK(v.size() == 5);
    CHECK_NEAR(v[0].x, 0.0f, 1e-5);
    CHECK(v[1].x < 0.0f && v[2].x > 0.0f);
    CHECK(v[1].y > 0.0f && v[3].y > v[1].y);

    // Vertical line descends.
    auto vline = formationOffsets(FormationShape::VLine, 3, 30.0f);
    CHECK(vline.size() == 3);
    CHECK_NEAR(vline[1].y, 30.0f, 1e-4);
    CHECK_NEAR(vline[2].y, 60.0f, 1e-4);

    // Arc members all sit within the radius band.
    auto arc = formationOffsets(FormationShape::Arc, 6, 36.0f);
    CHECK(arc.size() == 6);
    for (const auto& p : arc) CHECK(length(p) < 100.0f);

    // Shape names map correctly.
    CHECK(formationShapeFromName("v_formation") == FormationShape::VFormation);
    CHECK(formationShapeFromName("wave_left") == FormationShape::SideWaveLeft);
    CHECK(formationShapeFromName("nonsense") == FormationShape::Single);
}
