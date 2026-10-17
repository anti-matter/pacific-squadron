#include "test_framework.h"

int main() {
    test_movement();
    test_collision();
    test_weapons();
    test_formation();
    test_scoring();
    test_levelscript();
    test_boss();
    test_boss_natural_spawn();
    test_boss_slot_pinning();
    test_formation_edge_despawn();
    test_ground_scroll_anchor();
    test_menu_quit();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
