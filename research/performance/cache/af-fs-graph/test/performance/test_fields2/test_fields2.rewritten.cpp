#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <iostream>
#include <thread>
#include <vector>

const int NUM_THREADS = 4;
long ITERATIONS = 10; // Default preserved; measurement CLI can extend both versions equally.

// ==========================================
// 结构体 1: ComplexEntity (39 个字段，模拟角色实体)
// ==========================================
struct ComplexEntity {
    volatile long int health;       // 热
    volatile long int padding1[7];  // 强制挤出 Cacheline
    volatile long int mana;         // 热
    volatile long int padding2[7];
    volatile long int pos_x;        // 温
    volatile long int pos_y;        // 温
    volatile long int padding3[6];
    volatile long int velocity_x;   // 温
    volatile long int velocity_y;   // 温
    volatile long int score;        // 冷
    volatile long int level;        // 冷
    volatile long int experience;   // 冷
    volatile long int gold;         // 冷
    volatile long int padding_cold[10]; // 极冷
};

// ==========================================
// 结构体 2: GameConfig (36 个字段，模拟全局配置)
// ==========================================
struct GameConfig {
    volatile long int difficulty;   // 热
    volatile long int max_retries;  // 热
    volatile long int unused_flags1[10];
    volatile long int max_players;  // 温
    volatile long int timeout_ms;   // 温
    volatile long int unused_flags2[10];
    volatile long int spawn_rate;   // 冷
    volatile long int drop_rate;    // 冷
    volatile long int unused_flags3[10];
};

// ==========================================
// 结构体 3: WeaponStats (30 个字段，模拟装备数据)
// ==========================================
struct WeaponStats {
    volatile long int base_damage;     // 热
    volatile long int durability;      // 热
    volatile long int unused_pad1[8];
    volatile long int critical_chance; // 温
    volatile long int attack_speed;    // 温
    volatile long int unused_pad2[8];
    volatile long int element_type;    // 冷
    volatile long int weight;          // 冷
    volatile long int unused_pad3[8];
};

// ==========================================
// 30个 离散全局变量 (参考 test_singles)
// ==========================================




// ==========================================
// 多维数组与全局实例
// ==========================================
const int N = 16;
const int M = 8;
const int P = 4;

GameConfig global_cfg;
struct alignas(64) EntityLayout {
    volatile long health[M][N];
    unsigned char soa_gap[64];
    ComplexEntity residual[M][N]; // Preserve original struct stride and selected-member hole.
};
static_assert(offsetof(EntityLayout, residual) == 1088);
EntityLayout entity_layout;
struct alignas(64) WeaponLayout {
    volatile long base_damage[M][N];
    unsigned char soa_gap[64];
    WeaponStats residual[M][N]; // Preserve original struct stride and selected-member hole.
};
static_assert(offsetof(WeaponLayout, residual) == 1088);
WeaponLayout weapon_layout;
volatile long int matrix_2d[M][N];
volatile long int tensor_3d[M][N][P];


static_assert(N == 16 && M == 8 && P == 4 && NUM_THREADS == 4);
static_assert(sizeof(long) == 8 && sizeof(ComplexEntity) == 320 && sizeof(WeaponStats) == 240);
static_assert(std::atomic_ref<volatile long>::is_always_lock_free);
static_assert(alignof(GameConfig) >= std::atomic_ref<volatile long>::required_alignment);
// Shared safety repair in BOTH variants: retain all original config operations.
static void config_add(volatile long &field, long value) {
    std::atomic_ref<volatile long>(field).fetch_add(value, std::memory_order_relaxed);
}
static long config_load(volatile long &field) {
    return std::atomic_ref<volatile long>(field).load(std::memory_order_relaxed);
}


struct alignas(64) PackedGlobals {
    volatile long g_var_0;
    volatile long g_var_1;
    volatile long g_var_2;
    volatile long g_var_3;
    unsigned char gap_32[32];
    volatile long g_var_10;
    volatile long g_var_11;
    volatile long g_var_8;
    volatile long g_var_9;
    unsigned char gap_96[32];
    volatile long g_var_12;
    volatile long g_var_13;
    volatile long g_var_14;
    volatile long g_var_15;
    unsigned char gap_160[32];
    volatile long g_var_4;
    volatile long g_var_5;
    volatile long g_var_6;
    volatile long g_var_7;
};
PackedGlobals packed_globals;
static_assert(offsetof(PackedGlobals, g_var_0) == 0);
static_assert(offsetof(PackedGlobals, g_var_1) == 8);
static_assert(offsetof(PackedGlobals, g_var_2) == 16);
static_assert(offsetof(PackedGlobals, g_var_3) == 24);
static_assert(offsetof(PackedGlobals, g_var_4) == 192);
static_assert(offsetof(PackedGlobals, g_var_5) == 200);
static_assert(offsetof(PackedGlobals, g_var_6) == 208);
static_assert(offsetof(PackedGlobals, g_var_7) == 216);
static_assert(offsetof(PackedGlobals, g_var_8) == 80);
static_assert(offsetof(PackedGlobals, g_var_9) == 88);
static_assert(offsetof(PackedGlobals, g_var_10) == 64);
static_assert(offsetof(PackedGlobals, g_var_11) == 72);
static_assert(offsetof(PackedGlobals, g_var_12) == 128);
static_assert(offsetof(PackedGlobals, g_var_13) == 136);
static_assert(offsetof(PackedGlobals, g_var_14) == 144);
static_assert(offsetof(PackedGlobals, g_var_15) == 152);
volatile long g_var_16, g_var_17, g_var_18, g_var_19, g_var_20, g_var_21, g_var_22, g_var_23, g_var_24, g_var_25, g_var_26, g_var_27, g_var_28, g_var_29;
void worker(int thread_id) {
    for (int iter = 0; iter < ITERATIONS; ++iter) {

        // --- A. GameConfig 访问 ---
        config_add(global_cfg.difficulty, 1);
        config_add(global_cfg.max_retries, 1);
        if (iter % 2 == 0) {
            config_add(global_cfg.max_players, thread_id);
            config_add(global_cfg.timeout_ms, 10);
        }
        if (iter % 5 == 0) {
            config_add(global_cfg.spawn_rate, 1);
            config_add(global_cfg.drop_rate, 1);
        }

        // --- B. 测试多个结构体数组 (ComplexEntity & WeaponStats) ---
        for (int i = 0; i < N; i++) {
            // 频次等级 1: 高频 (每轮)
            entity_layout.health[thread_id][i] += config_load(global_cfg.difficulty);
            entity_layout.residual[thread_id][i].mana -= 1;

            weapon_layout.base_damage[thread_id][i] += 5;
            weapon_layout.residual[thread_id][i].durability -= 1;

            // 频次等级 2: 中频 (1/2)
            if (iter % 2 == 0) {
                entity_layout.residual[thread_id][i].pos_x += thread_id;
                entity_layout.residual[thread_id][i].pos_y += entity_layout.residual[thread_id][i].pos_x;
                entity_layout.residual[thread_id][i].velocity_x += 1;
                entity_layout.residual[thread_id][i].velocity_y += 1;

                weapon_layout.residual[thread_id][i].critical_chance += 1;
                weapon_layout.residual[thread_id][i].attack_speed += 2;
            }

            // 频次等级 3: 低频 (1/5)
            if (iter % 5 == 0) {
                entity_layout.residual[thread_id][i].score += 10;
                entity_layout.residual[thread_id][i].level += 1;
                entity_layout.residual[thread_id][i].experience += 5;
                entity_layout.residual[thread_id][i].gold += 1;

                weapon_layout.residual[thread_id][i].element_type = 1;
                weapon_layout.residual[thread_id][i].weight += 1;
            }
        }

        // --- C. 测试单纯多维数组 (触发 dim0/dim1 维度的亲和性算法) ---
        for (int i = 0; i < N; i++) {
            matrix_2d[thread_id][i] += 1;
            for (int k = 0; k < P; k++) {
                tensor_3d[thread_id][i][k] += 1;
            }
        }

        // --- D. 测试大量离散全局变量 ---
        if (thread_id == 0) {
            packed_globals.g_var_0++; packed_globals.g_var_1++; packed_globals.g_var_2++; packed_globals.g_var_3++;
        } else if (thread_id == 1) {
            packed_globals.g_var_4++; packed_globals.g_var_5++; packed_globals.g_var_6++; packed_globals.g_var_7++;
        } else if (thread_id == 2) {
            packed_globals.g_var_8++; packed_globals.g_var_9++; packed_globals.g_var_10++; packed_globals.g_var_11++;
        } else if (thread_id == 3) {
            packed_globals.g_var_12++; packed_globals.g_var_13++; packed_globals.g_var_14++; packed_globals.g_var_15++;
        }
        // g_var_16 到 g_var_29 故意打入冷宫，测试冷热剥离
    }
}

std::vector<long> snapshot() {
    std::vector<long> out;
    auto add = [&](long value) { out.push_back(value); };
    for (int i = 0; i < N; ++i) for (int t = 0; t < M; ++t) {
        add(entity_layout.health[t][i]);
        for (int k = 0; k < 7; ++k) add(entity_layout.residual[t][i].padding1[k]);
        add(entity_layout.residual[t][i].mana);
        for (int k = 0; k < 7; ++k) add(entity_layout.residual[t][i].padding2[k]);
        add(entity_layout.residual[t][i].pos_x);
        add(entity_layout.residual[t][i].pos_y);
        for (int k = 0; k < 6; ++k) add(entity_layout.residual[t][i].padding3[k]);
        add(entity_layout.residual[t][i].velocity_x);
        add(entity_layout.residual[t][i].velocity_y);
        add(entity_layout.residual[t][i].score);
        add(entity_layout.residual[t][i].level);
        add(entity_layout.residual[t][i].experience);
        add(entity_layout.residual[t][i].gold);
        for (int k = 0; k < 10; ++k) add(entity_layout.residual[t][i].padding_cold[k]);
        add(weapon_layout.base_damage[t][i]);
        add(weapon_layout.residual[t][i].durability);
        for (int k = 0; k < 8; ++k) add(weapon_layout.residual[t][i].unused_pad1[k]);
        add(weapon_layout.residual[t][i].critical_chance);
        add(weapon_layout.residual[t][i].attack_speed);
        for (int k = 0; k < 8; ++k) add(weapon_layout.residual[t][i].unused_pad2[k]);
        add(weapon_layout.residual[t][i].element_type);
        add(weapon_layout.residual[t][i].weight);
        for (int k = 0; k < 8; ++k) add(weapon_layout.residual[t][i].unused_pad3[k]);
        add(matrix_2d[t][i]);
        for (int k = 0; k < P; ++k) add(tensor_3d[t][i][k]);
    }
    add(config_load(global_cfg.difficulty));
    add(config_load(global_cfg.max_retries));
    for (int k = 0; k < 10; ++k) add(global_cfg.unused_flags1[k]);
    add(config_load(global_cfg.max_players));
    add(config_load(global_cfg.timeout_ms));
    for (int k = 0; k < 10; ++k) add(global_cfg.unused_flags2[k]);
    add(config_load(global_cfg.spawn_rate));
    add(config_load(global_cfg.drop_rate));
    for (int k = 0; k < 10; ++k) add(global_cfg.unused_flags3[k]);
    add(packed_globals.g_var_0);
    add(packed_globals.g_var_1);
    add(packed_globals.g_var_2);
    add(packed_globals.g_var_3);
    add(packed_globals.g_var_4);
    add(packed_globals.g_var_5);
    add(packed_globals.g_var_6);
    add(packed_globals.g_var_7);
    add(packed_globals.g_var_8);
    add(packed_globals.g_var_9);
    add(packed_globals.g_var_10);
    add(packed_globals.g_var_11);
    add(packed_globals.g_var_12);
    add(packed_globals.g_var_13);
    add(packed_globals.g_var_14);
    add(packed_globals.g_var_15);
    add(g_var_16);
    add(g_var_17);
    add(g_var_18);
    add(g_var_19);
    add(g_var_20);
    add(g_var_21);
    add(g_var_22);
    add(g_var_23);
    add(g_var_24);
    add(g_var_25);
    add(g_var_26);
    add(g_var_27);
    add(g_var_28);
    add(g_var_29);
    return out;
}
std::vector<std::pair<std::string, uintptr_t>> runtime_addresses() {
    std::vector<std::pair<std::string, uintptr_t>> out;
    auto add = [&](const char *name, const volatile long &value) {
        out.emplace_back(name, reinterpret_cast<uintptr_t>(&value));
    };
    add("entity_health_0_0", entity_layout.health[0][0]);
    add("entity_health_1_0", entity_layout.health[0][1]);
    add("entity_health_0_1", entity_layout.health[1][0]);
    add("entity_mana_0_0", entity_layout.residual[0][0].mana);
    add("entity_mana_1_0", entity_layout.residual[0][1].mana);
    add("entity_mana_0_1", entity_layout.residual[1][0].mana);
    add("weapon_damage_0_0", weapon_layout.base_damage[0][0]);
    add("weapon_damage_1_0", weapon_layout.base_damage[0][1]);
    add("weapon_damage_0_1", weapon_layout.base_damage[1][0]);
    add("matrix_0_0", matrix_2d[0][0]);
    add("matrix_1_0", matrix_2d[0][1]);
    add("matrix_0_1", matrix_2d[1][0]);
    add("tensor_0_0", tensor_3d[0][0][0]);
    add("tensor_1_0", tensor_3d[0][1][0]);
    add("tensor_0_1", tensor_3d[1][0][0]);
    add("g_var_0", packed_globals.g_var_0);
    add("g_var_1", packed_globals.g_var_1);
    add("g_var_2", packed_globals.g_var_2);
    add("g_var_3", packed_globals.g_var_3);
    add("g_var_4", packed_globals.g_var_4);
    add("g_var_5", packed_globals.g_var_5);
    add("g_var_6", packed_globals.g_var_6);
    add("g_var_7", packed_globals.g_var_7);
    add("g_var_8", packed_globals.g_var_8);
    add("g_var_9", packed_globals.g_var_9);
    add("g_var_10", packed_globals.g_var_10);
    add("g_var_11", packed_globals.g_var_11);
    add("g_var_12", packed_globals.g_var_12);
    add("g_var_13", packed_globals.g_var_13);
    add("g_var_14", packed_globals.g_var_14);
    add("g_var_15", packed_globals.g_var_15);
    return out;
}

#include "test_fields2_support.hpp"
int main(int argc, char **argv) { return run_test_fields2(argc, argv); }
