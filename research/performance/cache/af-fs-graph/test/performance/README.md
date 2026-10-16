# test_fields2 源码改写与性能验证

完整对照以 `benchmark/test_fields2.cpp` 为输入，保留 A-D 全部 worker 操作。
[基线版](test_fields2/test_fields2.baseline.cpp) 与 [布局改写版](test_fields2/test_fields2.rewritten.cpp)
均可独立编译；[结果与边界](TEST_FIELDS2_RESULTS.md) 说明实测范围。

## 完整源码对照

从 af-fs-graph 根目录执行，需要 Linux、Python 3、C++20/pthread 和四个可用 CPU。
默认编译器为 `/usr/local/llvm-19.1.7/bin/clang++`，可用 `--compiler` 覆盖。

```bash
python3 test/performance/run_test_fields2.py \
  --output test/artifacts/my-test-fields2 --samples 11 --target-ms 200
python3 test/performance/run_test_fields2.py \
  --check-only test/artifacts/my-test-fields2/results.json
```

默认从 [固定布局配置](fixtures/layout_decisions.json) 读取已验收判决及原文件 SHA256，
不依赖之前的运行产物。适配器只支持该固定夹具的已注明布局，输入源码改变时须先审查适配器。
`--decisions` 可指定新生成的真实验收目录；`--cpus 0,1,2,3` 可指定四个允许绑定的不同 CPU。
输出目录必须新建或为空。

适配器生成原文副本、完整基线、完整改写、公共修复 diff 和纯布局 diff，再以相同选项编译。
两版共同使用 `atomic_ref<volatile long>` 修复原 GameConfig 数据竞争，保留配置更新和逐元素读取。
health 仍随线程调度变化，检查其合法范围；其余确定性字段按独立公式逐值核验，含冷字段、
显式 padding、未访问列和全部全局变量。每样本还核验 CPU、迭代数、实际地址/stride 及全局 offset。
该验证不构成全部 health 调度行为的等价性证明。

每个样本运行独立进程，固定四个工作 CPU，交替原→改写/改写→原，默认每项 11 对。
原始 10 轮与延长迭代分别报告；先探测校准延长迭代数，两版始终使用相同工作量。
计时从线程创建前到 join 后，包括完整 worker，排除输出与状态检查，不做进程内预热。
保留全部样本，不剔除离群值，给出耗时中位数、配对加速比中位数和固定种子 bootstrap 区间。
两个耗时中位数之比与配对比值中位数不同；区间跨 1 时不能据此确认稳定收益。

可直接编译实际改写文件：

```bash
/usr/local/llvm-19.1.7/bin/clang++ -std=c++20 -O3 -pthread -Wno-deprecated-volatile \
  test/performance/test_fields2/test_fields2.rewritten.cpp -o /tmp/test-fields2-rewritten
/tmp/test-fields2-rewritten 10 0 1 2 3
```

## 布局微基准

`layout_bench.cpp` 是早期提取负载的分项实验：移除 GameConfig 竞争，以 `iter+1` 替代 health 输入，
分别测试二维/三维数组、两个结构体数组、全局变量和组合。它的结果不能替代完整源码结果。
[历史摘要](RESULTS.md) 保留已测数值。用新的分析产物运行：

```bash
python3 test/run_all_tests.py --real --output test/artifacts/perf-input
python3 test/performance/run_performance.py \
  --decisions test/artifacts/perf-input \
  --output test/artifacts/my-layout-performance --samples 11 --target-ms 200
python3 test/performance/check_results.py test/artifacts/my-layout-performance/results.json
```

分项计时使用 512 轮预热、统一发令并等待四线程完成；分配、线程创建和 join 位于计时外。
因此其计时边界也不同于完整源码对照。可用固定测试头文件单独编译：

```bash
/usr/local/llvm-19.1.7/bin/clang++ -std=c++20 -O3 -pthread -Wno-deprecated-volatile \
  -I test/performance/fixtures test/performance/layout_bench.cpp -o /tmp/af-fs-layout-bench
/tmp/af-fs-layout-bench matrix_2d original 1000000 0 1 2 3
/tmp/af-fs-layout-bench matrix_2d rewritten 1000000 0 1 2 3
```

## 仓库边界

入库内容包括源码、固定配置和简明方法/结果摘要。构建、二进制、CSV/DOT、计时 JSON、完整状态快照、
原文副本和生成 diff 保留在本地并由 .gitignore 排除；这些内容可通过上述入口重建。
`test/fixtures/current-baseline/` 的小型真实 Trace 是确定性回归输入，作为必要测试夹具单独保留。
模型按 64B 放置，虚拟机物理缓存行与频率未独立确认；本环境没有可用硬件 PMU，不能报告真实 miss 数量。
