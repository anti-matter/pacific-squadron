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
    test_sfx_explosions();
    test_menu_quit();
    test_menu_move_sfx();
    test_menu_confirm_sfx();
    test_shipped_data();
    test_input_gamepad_axes();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
