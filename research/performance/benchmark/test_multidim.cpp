#include <iostream>
#include <thread>
#include <vector>

const int NUM_THREADS = 4;
const int ITERATIONS = 100000; // 增加循环次数，产生大量访存

// 为了保证同一行的元素落在同一个 Cacheline，M 至少要大于等于线程数
const int N = 32;
const int M = 8; 
const int P = 4;

// 1. 二维数组全局变量
volatile long int global_2d[N][M];

// 2. 三维数组全局变量
volatile long int global_3d[N][M][P];

// 3. 结构体数组全局变量
struct DataNode {
    volatile long int val;
    volatile long int flag;
};
DataNode global_struct_arr[N][M];

// 线程工作函数：多线程跨步访问引发 False Sharing
void worker(int thread_id) {
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        
        // 【二维数组】
        // Thread 0 访问 [i][0], Thread 1 访问 [i][1] ...
        // [i][0] 和 [i][1] 是相邻的 8 字节，绝对在同一个 64 字节的 Cacheline 内。
        // 多核同时并发写入，产生极其密集的 False Sharing (分类 ② 甚至 ①)。
        for (int i = 0; i < N; ++i) {
            global_2d[i][thread_id] += 1;
        }

        // 【三维数组】
        // Thread 0 访问 [i][0][0], Thread 1 访问 [i][1][0] ...
        // 跨越 32 字节，但仍然在同一个 Cacheline 或引发强烈的关联竞争
        for (int k = 0; k < P; ++k) {
            for (int i = 0; i < N; ++i) {
                global_3d[i][thread_id][k] += 1;
            }
        }

        // 【结构体数组】
        // DataNode 占用 16 字节。
        // Thread 0 写 [i][0], Thread 1 写 [i][1] (相邻16字节，落在同一 Cacheline)
        for (int i = 0; i < N; ++i) {
            global_struct_arr[i][thread_id].val += 1;
            global_struct_arr[i][thread_id].flag -= 1;
        }
    }
}

int main() {
    std::cout << "🚀 开始执行多线程多维数组并发测试 (制造海量 False Sharing)..." << std::endl;
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "✅ 多线程并发测试完成！" << std::endl;
    return 0;
}
