# AF/FS benchmark 说明

本目录包含两类用途不同的程序：

- `test_*.cpp`：给 `af-fs-graph` 流水线生成访存 Trace，验证数组轴、结构体字段和全局变量布局判决；
- `sharing_bench*`：给外部计时或 perfparse 使用，不能直接当作图分析回归夹具。

## 主流水线

当前仓库路径不是 Makefile 默认的 `/her/alicewiki` 时，需要覆盖 `awiki_path`：

```bash
make awiki_path=/workspace/Ascend-env/alicewiki SRCS=test_fields2.cpp
make awiki_path=/workspace/Ascend-env/alicewiki SRCS=test_fields2.cpp run
make awiki_path=/workspace/Ascend-env/alicewiki SRCS=test_fields2.cpp opt
make awiki_path=/workspace/Ascend-env/alicewiki SRCS=test_fields2.cpp report
make awiki_path=/workspace/Ascend-env/alicewiki SRCS=test_fields2.cpp verify
```

默认分析参数为 `window=30000ns`、`k=1`、`g=15`、`gamma=10`、`line=64`。
时间窗使用墙钟纳秒，换机器、编译优化级别或采样率后必须重新标定。

LLVM 插件和运行时默认从 `$(affspath)/field_analysis/build` 读取；外部 DWARF 工具可用
`DBG_VIEWTYPE=/path/to/dbg-viewtype` 覆盖。

`verify` 只统计报告中的动作数量；严格的数学与格式回归在
`cache/af-fs-graph/test/run_all_tests.py`。


## test_fields2 的完整源码性能对照

`test_fields2.cpp` 保留为原始 Trace 夹具，其原始 GameConfig 读写存在非原子竞争。
完整性能对照另外生成保留 A-D 全部操作的两份 C++：两版共同用 atomic_ref 修复配置竞争，
布局改写版再应用字段拆分、轴置换与全局放置。原始 10 轮和延长迭代分别报告。
源码和实测结果见 [完整源码对照](../cache/af-fs-graph/test/performance/TEST_FIELDS2_RESULTS.md)。
