# 回归、固定回放与真实验收循环

普通入口自动构建四个 Rust 工具并执行 cargo test、Python/合成回归和固定真实 Trace 回放。
需要 Rust 和已缓存的依赖；cargo 使用 offline 模式。失败、缺失二进制和跳过均不能算通过。

```bash
python3 test/run_all_tests.py
```

完整测试循环先执行上述门禁，再进行指定次数的全新 C++/LLVM 采样与分析。次数必须为正数，
任一轮失败立即停止，不重试掩盖失败、不自动刷新基准。输出目录必须为空。

```bash
python3 test/run_all_tests.py --real --real-runs 2 --output test/artifacts/my-loop
```

多轮目录包含 `loop.json`、`run-01/`、`run-02/`；每轮同时运行两个真实工作负载：

- 原 `test_fields2.cpp`：四个数组、三种结构体、离散全局变量及真实共享来源。
- `test/member_arrays.cpp`：二维成员数组、单实例/数组实例、char[3] 后的对齐空洞、合法的 padding 字节访问。
  元素访问必须保留下标；完整成员只打包一次；padding 字节不能变成数组元素；SOA 的字段 stride 必须为完整成员大小。

循环中每轮用独立公式校验本轮分数，不跨不同 Trace 硬比较绝对分数。
单轮仍支持 `python3 test/run_all_tests.py --real --output test/artifacts/my-run`。
需要 CMake、LLVM 19、C++ 静态链接库、libdwarf/libelf 开发文件和 Make。
默认 LLVM 安装为 `/usr/local/llvm-19.1.7`，可用 `LLVM_ROOT` 覆盖；插件头文件和工具使用同一安装。

`pipeline_checks.py` 独立检查子图闭合、四类计数、共享来源、候选地址/字段 stride、缓存行范围、
全图分数和最优候选、11 个诊断值/配对数、四个数组的原始共享行、冷字段完整性及物理放置。
新增成员工作负载还核验 18 个数组叶节点、4 个完整字段和原始 Trace 中的实际地址/访问大小。

各轮保留 `trace.csv`、`graph.*`、报告、构建日志和 `acceptance.json`，新增负载在 `members/` 中。
可重验已有两负载产物：

```bash
python3 test/run_real_e2e.py --check-only test/artifacts/my-loop/run-01
```

## 固定真实输入（S2-0）

`fixtures/current-baseline/` 保存压缩的 55379 行真实 Trace、对应 DWARF 类型/变量转储和只读预期值。
该回归不依赖原 ELF 或重新采样；先校验输入 SHA256，再完整运行 Generator、打包、候选评分和报告。
核验全部 DOT/文本布局哈希、47 个候选及最终判决，同时独立复算 11 个诊断值/配对数和四个数组的几何共享行。
不提供自动接受差异或自动覆盖预期值的参数；基准变更须经人工审查。

```bash
python3 test/replay_baseline.py
```

历史 `baseline.json` 保留旧模型的 11 个分数，但其 55361 行原始 Trace 缺失。它只作历史元数据，
当前回归明确用上述持久新基准替代，不声称重放了旧 Trace。

## 解析范围

地址必须落在已知对象及其成员/叶元素范围内，且整个访问范围有效。未知对象、隐式 padding、
跨成员或跨叶元素的批量访问、零长度及溢出范围会跳过；日志报告解析/跳过计数，没有可解析访问则失败。
数组成员元素保留独立访问节点，通过完整字段元数据汇聚打包及 partial SOA；不会用下一成员间距推断元素大小。

上述分析验收证明源码链路和图模型计算通过；推荐布局的实际 C++ 实现与墙钟对照另见
[performance/README.md](performance/README.md) 和 [实测报告](performance/RESULTS.md)。
性能入口通过 checksum 和独立公式验证状态，记录全部原/改写计时；不提供通用自动源码改写。
partial SOA 保留残余 AoS 原 stride/空洞；轴候选覆盖每轴外移/内移，不穷举所有排列。


## 完整 test_fields2 源码对照

保留 `test_fields2.cpp` 的 A-D 全部操作，将配置竞争用相同原子操作共同修复，
再只对其中一版应用布局改写。完整基线/改写源码、两份 diff 和可复核的原始状态见
[performance/TEST_FIELDS2_RESULTS.md](performance/TEST_FIELDS2_RESULTS.md)。
原始 10 轮与延长迭代分开计时，不再用布局微基准替代完整源程序验收。

```bash
python3 test/performance/run_test_fields2.py --output test/artifacts/my-full-fields2
```
