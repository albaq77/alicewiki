// Measurement only: the complete worker body remains in each .cpp file.
#pragma once
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>

int run_test_fields2(int argc, char **argv) {
    try {
        if (argc != 1 && argc != 6)
            throw std::runtime_error("usage: test_fields2 [iterations cpu0 cpu1 cpu2 cpu3]");
        cpu_set_t allowed;
        if (sched_getaffinity(0, sizeof(allowed), &allowed))
            throw std::runtime_error("cannot read CPU affinity");
        std::array<int, NUM_THREADS> cpus{};
        if (argc == 6) {
            ITERATIONS = std::stol(argv[1]);
            for (int t = 0; t < NUM_THREADS; ++t) cpus[t] = std::stoi(argv[t + 2]);
        } else {
            int t = 0;
            for (int cpu = 0; cpu < CPU_SETSIZE && t < NUM_THREADS; ++cpu)
                if (CPU_ISSET(cpu, &allowed)) cpus[t++] = cpu;
            if (t != NUM_THREADS) throw std::runtime_error("four CPUs required");
        }
        if (ITERATIONS < 1 || ITERATIONS > 2000000)
            throw std::runtime_error("iterations must be in [1,2000000]");
        for (int t = 0; t < NUM_THREADS; ++t) {
            if (cpus[t] < 0 || cpus[t] >= CPU_SETSIZE || !CPU_ISSET(cpus[t], &allowed))
                throw std::runtime_error("invalid CPU");
            for (int other = 0; other < t; ++other)
                if (cpus[t] == cpus[other]) throw std::runtime_error("distinct CPUs required");
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed) && std::find(cpus.begin(), cpus.end(), cpu) == cpus.end()) {
                cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
                if (sched_setaffinity(0, sizeof(mask), &mask))
                    throw std::runtime_error("cannot bind controller");
                break;
            }
        }
        std::atomic<bool> affinity_failed{false};
        std::array<int, NUM_THREADS> observed{};
        std::vector<std::thread> threads;
        // Match the original main: include creation, the entire A-D worker,
        // and join. No workload phase is removed, and there is no in-process warmup.
        auto start = std::chrono::steady_clock::now();
        for (int t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([&, t] {
                cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpus[t], &mask);
                if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask)) affinity_failed = true;
                observed[t] = sched_getcpu();
                worker(t);
            });
        }
        for (auto &thread : threads) thread.join();
        auto stop = std::chrono::steady_clock::now();
        if (affinity_failed || observed != cpus) throw std::runtime_error("worker binding failed");
        auto state = snapshot();
        std::cout << std::setprecision(17)
                  << "{\"seconds\":" << std::chrono::duration<double>(stop - start).count()
                  << ",\"iterations\":" << ITERATIONS << ",\"cpus\":[";
        for (int t = 0; t < NUM_THREADS; ++t) std::cout << (t ? "," : "") << observed[t];
        std::cout << "],\"state\":[";
        for (size_t i = 0; i < state.size(); ++i) std::cout << (i ? "," : "") << state[i];
        std::cout << "],\"addresses\":{";
        bool first = true;
        for (const auto &[name, address] : runtime_addresses()) {
            std::cout << (first ? "" : ",") << '\"' << name << "\":" << address;
            first = false;
        }
        std::cout << "}}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
