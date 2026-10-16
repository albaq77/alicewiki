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
volatile long int g_var_0, g_var_1, g_var_2, g_var_3, g_var_4, g_var_5, g_var_6, g_var_7, g_var_8, g_var_9;
volatile long int g_var_10, g_var_11, g_var_12, g_var_13, g_var_14, g_var_15, g_var_16, g_var_17, g_var_18, g_var_19;
volatile long int g_var_20, g_var_21, g_var_22, g_var_23, g_var_24, g_var_25, g_var_26, g_var_27, g_var_28, g_var_29;

// ==========================================
// 多维数组与全局实例
// ==========================================
const int N = 16;
const int M = 8;
const int P = 4;

GameConfig global_cfg;
ComplexEntity entity_arr[N][M];
WeaponStats weapon_arr[N][M];
volatile long int matrix_2d[N][M];
volatile long int tensor_3d[N][M][P];


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
            entity_arr[i][thread_id].health += config_load(global_cfg.difficulty);
            entity_arr[i][thread_id].mana -= 1;

            weapon_arr[i][thread_id].base_damage += 5;
            weapon_arr[i][thread_id].durability -= 1;

            // 频次等级 2: 中频 (1/2)
            if (iter % 2 == 0) {
                entity_arr[i][thread_id].pos_x += thread_id;
                entity_arr[i][thread_id].pos_y += entity_arr[i][thread_id].pos_x;
                entity_arr[i][thread_id].velocity_x += 1;
                entity_arr[i][thread_id].velocity_y += 1;

                weapon_arr[i][thread_id].critical_chance += 1;
                weapon_arr[i][thread_id].attack_speed += 2;
            }

            // 频次等级 3: 低频 (1/5)
            if (iter % 5 == 0) {
                entity_arr[i][thread_id].score += 10;
                entity_arr[i][thread_id].level += 1;
                entity_arr[i][thread_id].experience += 5;
                entity_arr[i][thread_id].gold += 1;

                weapon_arr[i][thread_id].element_type = 1;
                weapon_arr[i][thread_id].weight += 1;
            }
        }

        // --- C. 测试单纯多维数组 (触发 dim0/dim1 维度的亲和性算法) ---
        for (int i = 0; i < N; i++) {
            matrix_2d[i][thread_id] += 1;
            for (int k = 0; k < P; k++) {
                tensor_3d[i][thread_id][k] += 1;
            }
        }

        // --- D. 测试大量离散全局变量 ---
        if (thread_id == 0) {
            g_var_0++; g_var_1++; g_var_2++; g_var_3++;
        } else if (thread_id == 1) {
            g_var_4++; g_var_5++; g_var_6++; g_var_7++;
        } else if (thread_id == 2) {
            g_var_8++; g_var_9++; g_var_10++; g_var_11++;
        } else if (thread_id == 3) {
            g_var_12++; g_var_13++; g_var_14++; g_var_15++;
        }
        // g_var_16 到 g_var_29 故意打入冷宫，测试冷热剥离
    }
}

std::vector<long> snapshot() {
    std::vector<long> out;
    auto add = [&](long value) { out.push_back(value); };
    for (int i = 0; i < N; ++i) for (int t = 0; t < M; ++t) {
        add(entity_arr[i][t].health);
        for (int k = 0; k < 7; ++k) add(entity_arr[i][t].padding1[k]);
        add(entity_arr[i][t].mana);
        for (int k = 0; k < 7; ++k) add(entity_arr[i][t].padding2[k]);
        add(entity_arr[i][t].pos_x);
        add(entity_arr[i][t].pos_y);
        for (int k = 0; k < 6; ++k) add(entity_arr[i][t].padding3[k]);
        add(entity_arr[i][t].velocity_x);
        add(entity_arr[i][t].velocity_y);
        add(entity_arr[i][t].score);
        add(entity_arr[i][t].level);
        add(entity_arr[i][t].experience);
        add(entity_arr[i][t].gold);
        for (int k = 0; k < 10; ++k) add(entity_arr[i][t].padding_cold[k]);
        add(weapon_arr[i][t].base_damage);
        add(weapon_arr[i][t].durability);
        for (int k = 0; k < 8; ++k) add(weapon_arr[i][t].unused_pad1[k]);
        add(weapon_arr[i][t].critical_chance);
        add(weapon_arr[i][t].attack_speed);
        for (int k = 0; k < 8; ++k) add(weapon_arr[i][t].unused_pad2[k]);
        add(weapon_arr[i][t].element_type);
        add(weapon_arr[i][t].weight);
        for (int k = 0; k < 8; ++k) add(weapon_arr[i][t].unused_pad3[k]);
        add(matrix_2d[i][t]);
        for (int k = 0; k < P; ++k) add(tensor_3d[i][t][k]);
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
    add(g_var_0);
    add(g_var_1);
    add(g_var_2);
    add(g_var_3);
    add(g_var_4);
    add(g_var_5);
    add(g_var_6);
    add(g_var_7);
    add(g_var_8);
    add(g_var_9);
    add(g_var_10);
    add(g_var_11);
    add(g_var_12);
    add(g_var_13);
    add(g_var_14);
    add(g_var_15);
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
    add("entity_health_0_0", entity_arr[0][0].health);
    add("entity_health_1_0", entity_arr[1][0].health);
    add("entity_health_0_1", entity_arr[0][1].health);
    add("entity_mana_0_0", entity_arr[0][0].mana);
    add("entity_mana_1_0", entity_arr[1][0].mana);
    add("entity_mana_0_1", entity_arr[0][1].mana);
    add("weapon_damage_0_0", weapon_arr[0][0].base_damage);
    add("weapon_damage_1_0", weapon_arr[1][0].base_damage);
    add("weapon_damage_0_1", weapon_arr[0][1].base_damage);
    add("matrix_0_0", matrix_2d[0][0]);
    add("matrix_1_0", matrix_2d[1][0]);
    add("matrix_0_1", matrix_2d[0][1]);
    add("tensor_0_0", tensor_3d[0][0][0]);
    add("tensor_1_0", tensor_3d[1][0][0]);
    add("tensor_0_1", tensor_3d[0][1][0]);
    add("g_var_0", g_var_0);
    add("g_var_1", g_var_1);
    add("g_var_2", g_var_2);
    add("g_var_3", g_var_3);
    add("g_var_4", g_var_4);
    add("g_var_5", g_var_5);
    add("g_var_6", g_var_6);
    add("g_var_7", g_var_7);
    add("g_var_8", g_var_8);
    add("g_var_9", g_var_9);
    add("g_var_10", g_var_10);
    add("g_var_11", g_var_11);
    add("g_var_12", g_var_12);
    add("g_var_13", g_var_13);
    add("g_var_14", g_var_14);
    add("g_var_15", g_var_15);
    return out;
}

#include "test_fields2_support.hpp"
int main(int argc, char **argv) { return run_test_fields2(argc, argv); }
