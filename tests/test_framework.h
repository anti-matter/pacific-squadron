#pragma once

#include <cmath>
#include <cstdio>
#include <string>

inline int g_checks = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                              \
    do {                                                                   \
        ++g_checks;                                                        \
        const double _va = static_cast<double>(a), _vb = static_cast<double>(b); \
        if (std::fabs(_va - _vb) > (double)(eps)) {                        \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d  %s ~= %s (%f vs %f)\n", __FILE__,     \
                        __LINE__, #a, #b, _va, _vb);                       \
        }                                                                  \
    } while (0)

void test_movement();
void test_collision();
void test_weapons();
void test_formation();
void test_scoring();
void test_levelscript();
void test_boss();
void test_boss_natural_spawn();
void test_boss_slot_pinning();
void test_formation_edge_despawn();
void test_ground_scroll_anchor();
void test_sfx_explosions();
void test_menu_quit();
void test_shipped_data();
