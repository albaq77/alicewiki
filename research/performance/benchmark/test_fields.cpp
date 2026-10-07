#include <stdio.h>
#include <stdlib.h>

// ==========================================
// 1. 定义多个结构体
// ==========================================
struct Point {
    int x;
    int y;
    int z;
};

struct Config {
    double threshold;
    int max_retries;
    char flags[8];
};

struct Player {
    int health;
    int mana;
    int score;
};

// ==========================================
// 2. 声明全局变量实例
// ==========================================
Point pt1 = {0, 0, 0};
Point pt2 = {0, 0, 0};
Config cfg = {1.5, 3, "abc"};

// ==========================================
// 3. 声明结构体数组
// ==========================================
Player team[10];

// ==========================================
// 4. 声明单独变量
// ==========================================
int global_counter = 0;
float global_temp = 98.6f;

// --- 测试场景 1：结构体内部强亲和 ---
// 预期：在 Point.fields.dot 中，Point::x 和 Point::y 会有极强的连线
void __attribute__((noinline)) loop_intra_struct() {
    for (int i = 0; i < 100000; ++i) {
        pt1.x += 1;
        pt1.y += pt1.x;
    }
}

// --- 测试场景 2：跨实例亲和 ---
// 预期：即使是不同的实例 (pt1 和 pt2)，由于归属同一类型，
// 在 Point.fields.dot 中会生成 Point::z 和 Point::x 的连线
void __attribute__((noinline)) loop_cross_instance() {
    for (int i = 0; i < 100000; ++i) {
        pt1.z += 1;
        pt2.x += pt1.z;
    }
}

// --- 测试场景 3：跨结构体数组亲和 ---
// 预期：在 Config.fields.dot 和 Player.fields.dot 中，
// 都会出现 Config::max_retries 和 Player::health 之间的跨界连线
void __attribute__((noinline)) loop_cross_struct() {
    for (int i = 0; i < 100000; ++i) {
        cfg.max_retries += 1;
        team[i % 10].health += cfg.max_retries;
    }
}

// --- 测试场景 4：单变量与结构体混合亲和 ---
// 预期：在 Player.fields.dot 和 Globals.fields.dot 中，
// 都会出现 global_counter 与 Player::score 的连线，并且
// 单变量 global_counter 不带任何 struct 前缀
void __attribute__((noinline)) loop_single_and_struct() {
    for (int i = 0; i < 100000; ++i) {
        global_counter += 1;
        team[i % 10].score += global_counter;
        global_temp += 0.1f;
    }
}

int main() {
    loop_intra_struct();
    loop_cross_instance();
    loop_cross_struct();
    loop_single_and_struct();
    
    printf("Test finished. Counter: %d, Temp: %f\n", global_counter, global_temp);
    return 0;
}
