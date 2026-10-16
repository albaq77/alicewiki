// Reuse the exact fixture types; its racing worker/main are never invoked.
#define main trace_fixture_main
#include "../../../../benchmark/test_fields2.cpp"
#undef main

#include "layout_config.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <memory>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>

static_assert(sizeof(ComplexEntity) == 320 && sizeof(WeaponStats) == 240);

struct Buffer {
    unsigned char *raw;
    explicit Buffer(size_t bytes) : raw(static_cast<unsigned char *>(
        ::operator new(bytes, std::align_val_t{64}))) { std::memset(raw, 0, bytes); }
    ~Buffer() { ::operator delete(raw, std::align_val_t{64}); }
    Buffer(const Buffer &) = delete;
};

template<class T, bool Changed, size_t Selected, size_t OriginalMod>
struct Objects {
    static constexpr size_t count = N * M;
    static constexpr size_t split_bytes = ((count * sizeof(long) + 63) / 64) * 64 + 64;
    Buffer memory{count * sizeof(T) + split_bytes + 128};
    T *aos;
    volatile long *split;
    Objects() {
        auto start = memory.raw + (Changed ? 0 : OriginalMod);
        aos = new (start + (Changed ? split_bytes : 0)) T[count]{};
        split = Changed ? new (start) long[count]{} : nullptr;
    }
    template<size_t Field> volatile long &get(int i, int t) {
        size_t index = Changed ? t * N + i : i * M + t;
        if constexpr (Changed && Field == Selected) return split[index];
        return *reinterpret_cast<volatile long *>(reinterpret_cast<unsigned char *>(aos + index) + Field);
    }
};

template<bool Changed> struct Data {
    Objects<ComplexEntity, Changed, ENTITY_FIELD, ENTITY_MOD> entity;
    Objects<WeaponStats, Changed, WEAPON_FIELD, WEAPON_MOD> weapon;
    Buffer matrix_memory{N * M * sizeof(long) + 128};
    Buffer tensor_memory{N * M * P * sizeof(long) + 128};
    Buffer globals_memory{2048};
    volatile long *matrix = new (matrix_memory.raw + MATRIX_MOD) long[N * M]{};
    volatile long *tensor = new (tensor_memory.raw + TENSOR_MOD) long[N * M * P]{};
    // Global fields are separately placed as long objects, matching graph.placement.txt.
    Data() {
        for (int g = 0; g < 16; ++g) new (globals_memory.raw + global_offset(g)) long{};
    }
    size_t global_offset(int g) const {
        return Changed ? GLOBAL_NEW[g] : GLOBAL_MOD + GLOBAL_OLD[g];
    }
    volatile long &global(int g) {
        return *reinterpret_cast<volatile long *>(globals_memory.raw + global_offset(g));
    }
    volatile long &mat(int i, int t) { return matrix[Changed ? t * N + i : i * M + t]; }
    volatile long &ten(int i, int t, int k) {
        return tensor[Changed ? (t * N + i) * P + k : (i * M + t) * P + k];
    }
};

#define E(field) d.entity.template get<offsetof(ComplexEntity, field)>(i, t)
#define W(field) d.weapon.template get<offsetof(WeaponStats, field)>(i, t)

template<bool Changed> inline void entity_step(Data<Changed> &d, int i, int t, long iter) {
    E(health) += iter + 1; // Deterministic input replacing racing global_cfg.difficulty.
    E(mana) -= 1;
    if (iter % 2 == 0) {
        E(pos_x) += t;
        E(pos_y) += E(pos_x);
        E(velocity_x) += 1;
        E(velocity_y) += 1;
    }
    if (iter % 5 == 0) {
        E(score) += 10; E(level) += 1; E(experience) += 5; E(gold) += 1;
    }
}

template<bool Changed> inline void weapon_step(Data<Changed> &d, int i, int t, long iter) {
    W(base_damage) += 5; W(durability) -= 1;
    if (iter % 2 == 0) { W(critical_chance) += 1; W(attack_speed) += 2; }
    if (iter % 5 == 0) { W(element_type) = 1; W(weight) += 1; }
}

// Preserve the original fixture's interleaving in the combined workload.
template<bool Changed> inline void pair_step(Data<Changed> &d, int i, int t, long iter) {
    E(health) += iter + 1; E(mana) -= 1;
    W(base_damage) += 5; W(durability) -= 1;
    if (iter % 2 == 0) {
        E(pos_x) += t; E(pos_y) += E(pos_x);
        E(velocity_x) += 1; E(velocity_y) += 1;
        W(critical_chance) += 1; W(attack_speed) += 2;
    }
    if (iter % 5 == 0) {
        E(score) += 10; E(level) += 1; E(experience) += 5; E(gold) += 1;
        W(element_type) = 1; W(weight) += 1;
    }
}

enum class Case { Matrix, Tensor, Entity, Weapon, Globals, Combined };

template<Case C, bool Changed> void kernel(Data<Changed> &d, int t, long iterations) {
    for (long iter = 0; iter < iterations; ++iter) {
        if constexpr (C == Case::Entity || C == Case::Weapon || C == Case::Combined)
            for (int i = 0; i < N; ++i) {
                if constexpr (C == Case::Combined) pair_step(d, i, t, iter);
                if constexpr (C == Case::Entity) entity_step(d, i, t, iter);
                if constexpr (C == Case::Weapon) weapon_step(d, i, t, iter);
            }
        if constexpr (C == Case::Matrix || C == Case::Tensor || C == Case::Combined)
            for (int i = 0; i < N; ++i) {
                if constexpr (C == Case::Matrix || C == Case::Combined) d.mat(i, t) += 1;
                if constexpr (C == Case::Tensor || C == Case::Combined)
                    for (int k = 0; k < P; ++k) d.ten(i, t, k) += 1;
            }
        if constexpr (C == Case::Globals || C == Case::Combined)
            for (int g = t * 4; g < t * 4 + 4; ++g) d.global(g) += 1;
    }
}

template<bool Changed> uint64_t checksum(Data<Changed> &d) {
    uint64_t hash = 1469598103934665603ULL;
    auto add = [&](long value) { hash = (hash ^ static_cast<uint64_t>(value)) * 1099511628211ULL; };
    // Read logical state in identical order, including untouched array elements.
    for (int i = 0; i < N; ++i) for (int t = 0; t < M; ++t) {
        add(E(health)); add(E(mana)); add(E(pos_x)); add(E(pos_y));
        add(E(velocity_x)); add(E(velocity_y)); add(E(score)); add(E(level));
        add(E(experience)); add(E(gold));
        add(W(base_damage)); add(W(durability)); add(W(critical_chance));
        add(W(attack_speed)); add(W(element_type)); add(W(weight));
        add(d.mat(i,t));
        for (int k = 0; k < P; ++k) add(d.ten(i,t,k));
    }
    for (int g = 0; g < 16; ++g) add(d.global(g));
    return hash;
}

template<Case C, bool Changed> void measure(long iterations, const std::array<int,4> &cpus) {
    auto data = std::make_unique<Data<Changed>>();
    std::atomic<int> ready{0}, done{0};
    std::atomic<bool> go{false};
    std::atomic<bool> affinity_failed{false};
    std::array<int,4> observed{};
    std::array<std::thread,4> workers;
    for (int t = 0; t < 4; ++t) workers[t] = std::thread([&, t] {
        cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpus[t], &mask);
        if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) affinity_failed = true;
        kernel<C>(*data, t, 512); // Warm pages/code/cache before timing.
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        observed[t] = sched_getcpu();
        kernel<C>(*data, t, iterations);
        done.fetch_add(1, std::memory_order_release);
        done.notify_one();
    });
    while (ready.load(std::memory_order_acquire) != 4) std::this_thread::yield();
    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    int completed = done.load(std::memory_order_acquire);
    while (completed != 4) { done.wait(completed); completed = done.load(std::memory_order_acquire); }
    auto stop = std::chrono::steady_clock::now();
    for (auto &thread : workers) thread.join();
    if (affinity_failed || observed != cpus) throw std::runtime_error("CPU binding failed");
    std::cout << "{\"seconds\":" << std::chrono::duration<double>(stop-start).count()
              << ",\"checksum\":\"" << checksum(*data) << "\",\"cpus\":["
              << observed[0] << ',' << observed[1] << ',' << observed[2] << ',' << observed[3] << "]}\n";
}

template<Case C> void variant(bool changed, long iterations, const std::array<int,4> &cpus) {
    if (changed) measure<C,true>(iterations, cpus); else measure<C,false>(iterations, cpus);
}

int main(int argc, char **argv) {
    try {
        if (argc != 8) throw std::runtime_error("usage: bench case original|rewritten iterations cpu0 cpu1 cpu2 cpu3");
        long iterations = std::stol(argv[3]);
        if (iterations <= 0 || iterations > 120000000) throw std::runtime_error("invalid iterations");
        std::array<int,4> cpus{};
        for (int t = 0; t < 4; ++t) {
            cpus[t] = std::stoi(argv[t+4]);
            if (cpus[t] < 0 || cpus[t] >= CPU_SETSIZE) throw std::runtime_error("invalid CPU");
        }
        cpu_set_t available;
        if (sched_getaffinity(0, sizeof(available), &available)) throw std::runtime_error("cannot read CPU affinity");
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            bool worker_cpu = false;
            for (int used : cpus) if (cpu == used) worker_cpu = true;
            if (CPU_ISSET(cpu, &available) && !worker_cpu) {
                cpu_set_t controller; CPU_ZERO(&controller); CPU_SET(cpu, &controller);
                if (sched_setaffinity(0, sizeof(controller), &controller)) throw std::runtime_error("cannot bind controller");
                break;
            }
        }
        std::cout << std::setprecision(17);
        std::string name = argv[1], mode = argv[2];
        if (mode != "original" && mode != "rewritten") throw std::runtime_error("invalid variant");
        bool changed = mode == "rewritten";
        if (name == "matrix_2d") variant<Case::Matrix>(changed,iterations,cpus);
        else if (name == "tensor_3d") variant<Case::Tensor>(changed,iterations,cpus);
        else if (name == "entity_arr") variant<Case::Entity>(changed,iterations,cpus);
        else if (name == "weapon_arr") variant<Case::Weapon>(changed,iterations,cpus);
        else if (name == "globals") variant<Case::Globals>(changed,iterations,cpus);
        else if (name == "combined") variant<Case::Combined>(changed,iterations,cpus);
        else throw std::runtime_error("invalid case");
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
