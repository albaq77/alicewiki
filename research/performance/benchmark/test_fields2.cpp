#include <iostream>
#include <thread>
#include <vector>

const int NUM_THREADS = 4;
const int ITERATIONS = 10; // 遵循用户要求：总访问量不要太大

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

void worker(int thread_id) {
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        
        // --- A. GameConfig 访问 ---
        global_cfg.difficulty += 1;
        global_cfg.max_retries += 1;
        if (iter % 2 == 0) {
            global_cfg.max_players += thread_id;
            global_cfg.timeout_ms += 10;
        }
        if (iter % 5 == 0) {
            global_cfg.spawn_rate += 1;
            global_cfg.drop_rate += 1;
        }

        // --- B. 测试多个结构体数组 (ComplexEntity & WeaponStats) ---
        for (int i = 0; i < N; i++) {
            // 频次等级 1: 高频 (每轮)
            entity_arr[i][thread_id].health += global_cfg.difficulty;
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

int main() {
    std::cout << "🚀 开始执行 test_fields2 (多重巨型结构体 + 多维数组 + 离散全局测试)..." << std::endl;
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "✅ test_fields2 测试完成！" << std::endl;
    return 0;
}
