
The project copied from https://github.com/albaq77/FieldAnalysis.

And some changes are not logged by git.



# Memory Access Sampler

基于 LLVM New Pass Manager 的轻量级 C/C++ 内存访问序列采样工具。
该工具去除了原始学术项目中冗余的分析、画图、以及 JSON 导出逻辑，专注于一件事情：
**高性能、全量地采集程序的内存访问序列，并直接输出极简的 CSV 格式**。

## 核心机制

1. **编译期插桩 (LLVM Pass)**
   工具会在编译阶段，通过遍历 LLVM IR，在 `Load`、`Store` 等访存指令前后强制插入函数调用 `__fa_record_access_v2`。

2. **运行时采集 (TLS Buffer + 批量刷盘)**
   运行时（`runtime/libaffinity.c`）为每个线程分配一块专属的 Thread-Local Storage 缓冲区。
   每一次访存触发时，工具无锁化地将访存事件收集到缓冲区中。
   Buffer 写满后，自动批量刷入磁盘，大幅降低 I/O 开销对业务程序性能的影响。

## 输出格式 (CSV)
生成的 trace 文件为 `.csv` 格式，每行严格包含 5 个逗号分隔的字段：
`[忽略的时间戳], [16进制内存地址], [R/W读写模式], [访问字节数大小], [CPU核心ID]`
例如：
`164324234567,0x408200,W,8,2`

此格式完美兼容 Rust 图计算生成器（`generator`）。

## 构建指南

依赖：LLVM (含 opt / clang), CMake

```bash
cmake -S . -B build -DLLVM_DIR=/path/to/llvm-build/lib/cmake/llvm
cmake --build build -j8
```

编译好后，您将得到：
1. **LLVM 插件**: `build/FieldAnalysisPass.so`
2. **运行时库**: `build/runtime/libaffinity.a`

在编译目标程序时加载插件并链接运行时即可抓取 Trace。
