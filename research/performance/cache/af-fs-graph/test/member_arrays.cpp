#include <thread>

// Member arrays, multi-dimensional indexing, and implicit inter-member padding.
// Each worker owns a separate instance; the program has no data races.
struct MemberEntity {
    volatile long values[2][2];
    volatile long tail;
};
struct PaddedMember {
    volatile char values[3];
    volatile long tail;
};

MemberEntity entities[2];
MemberEntity cfg;
PaddedMember padded[2];
volatile long marker;

void worker(int id) {
    for (int repeat = 0; repeat < 10; ++repeat) {
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                entities[id].values[i][j] += repeat + i + j;
        entities[id].tail += 1;
        for (int j = 0; j < 3; ++j)
            padded[id].values[j] += 1;
        padded[id].tail += 1;
        // Accessing an object's representation through unsigned char is valid.
        // This byte is implicit padding and must not become values[0] or [3].
        reinterpret_cast<volatile unsigned char *>(&padded[id])[3] = 0;
    }
}

int main() {
    std::thread a(worker, 0), b(worker, 1);
    a.join();
    b.join();
    cfg.values[1][1] = 7;
    cfg.tail = 8;
    marker = cfg.values[1][1];
}
