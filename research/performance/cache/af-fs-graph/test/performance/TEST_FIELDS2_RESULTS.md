# test_fields2 完整源码的实际对照

直接从 [原始 test_fields2.cpp](../../../../benchmark/test_fields2.cpp) 生成完整对照，
保留 A（GameConfig）、B（两个结构体数组）、C（二维/三维数组）、D（离散全局变量）的全部 worker 操作。

- [完整基线版](test_fields2/test_fields2.baseline.cpp)：原布局与全部操作，共同修复配置竞争并添加测量入口。
- [完整布局改写版](test_fields2/test_fields2.rewritten.cpp)：应用字段拆分、数组轴置换及全局放置。
- [公共计时入口](test_fields2/test_fields2_support.hpp)：创建线程前开始，全部 join 后结束。
- [源码适配器](test_fields2_sources.py)：生成原文副本、两版完整源码及公共修复/纯布局两份 diff。

原始文件保留为 Trace 夹具。原子引用修复在两版中完全一致：`atomic_ref<volatile long>` relaxed
实现原来的六个配置更新和逐元素 difficulty 读取，没有删除 A 段，也没有以固定值替代共享配置。
因此这里比较的是**共同修复配置竞争后的完整 test_fields2 工作负载**，不声称原始未定义数据竞争程序的语义等价加速。

实际布局为：entity `.health` 与 weapon `.base_damage` 拆成 `[thread][i]`；
残余 AoS 同时置换并保留原 stride/空洞；matrix/tensor 将 thread 轴提到最外；
16 个热全局变量按固定判决的 offset 放置，其余冷字段及 14 个冷全局变量保留。

## 实测摘要（2026-10-16）

Linux aarch64 虚拟机，Clang 19.1.7，`-std=c++20 -O3 -DNDEBUG -pthread`，未插桩。
四工作线程绑定 CPU 0/1/2/3，每项 11 对交替样本、每样本独立进程。
计时包含线程创建、亲和性设置、完整 worker 和 join，排除输出、快照及校验，不做进程内预热。

| 用例 | 每线程迭代 | 基线中位耗时 | 改写中位耗时 | 耗时下降 | 两个中位数之比 |
|---|---:|---:|---:|---:|---:|
| 原始迭代数 | 10 | 1.395 ms | 0.253 ms | 81.8% | 5.506× |
| 延长迭代 | 304230 | 694.235 ms | 194.411 ms | **72.0%** | **3.571×** |

长测逐对加速比中位数为 3.456×，95% bootstrap 区间 3.297–3.601×。
两个中位数之比与逐对比值中位数不是同一统计量。10 轮逐对区间为 0.450–7.349×，
受启动和调度波动主导，不能把其点估计当作稳定收益。未剔除任何样本，结果只适用于本次环境。
早期微基准的 80.2% 组合收益不能替代本表；模型 64B 假设也未通过宿主物理缓存行测量确认。
本环境 perf_event_open 返回 EPERM，尚无真实 miss 数量。

## 正确性与复跑

44 个正式状态，每个核验 9666 个逻辑值（共 425304 个值）。除 health 外的确定性字段逐值比较，
含完整配置、冷热字段、显式 padding、未访问列和全部全局变量。
health 仍依赖调度，逐热元素检查 `n*(n+1)/2 <= health <= 4*n*n`；该范围检查不证明完整调度语义等价。
每个进程输出实际地址，核验 stride、SOA 对齐和全局 offset。

两版以 ASan+UBSan 分别跑 10、1001 轮，4/4 通过；配置更新丢失、padding 污染、非法 health、
错误 stride 四类主动篡改均被拒绝。源码范围核查逆向还原公共原子修复及布局访问表达式，确认 A-D 未删减。

复跑入口与依赖见 [README.md](README.md)。原始计时、完整状态、构建日志和中间实验仅保留在本地，
由 .gitignore 排除；运行脚本会为每次实验保存源码 SHA256、编译命令、全部样本和两份源码 diff。
必要的默认布局输入已提取到 [固定配置](fixtures/layout_decisions.json)，新 checkout 无需旧性能产物即可运行。
