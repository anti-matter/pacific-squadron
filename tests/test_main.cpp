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
    test_bomb_sweep_and_boss_20pct();
    test_drydock_undocks_and_sails();
    test_yamato_turrets_track_player();
    test_ships_are_rammed_by_capital_ships();
    test_fortress_opens_its_bomb_bays();
    test_boss_slot_pinning();
    test_formation_edge_despawn();
    test_ground_scroll_anchor();
    test_sea_surface_fringe();
    test_pool_compaction_remaps_formations();
    test_sfx_explosions();
    test_menu_quit();
    test_menu_move_sfx();
    test_menu_confirm_sfx();
    test_shipped_data();
    test_input_gamepad_axes();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
