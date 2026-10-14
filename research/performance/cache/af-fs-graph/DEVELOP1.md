# DEVELOP1.md — 实施日志

- 阶段 0：`benchmark/Makefile`（改动文件 1 个）
- 阶段 1：止血，改动 7 个文件（见文末「阶段 1 实施日志」）

## 1. 改动清单

| ID | 原 | 现 |
|---|---|---|
| S0-2 | `package_fields/summarier/package_simple` 三处写死 `1 1 10 64` | `$(PARAMS)`，即 `K=1 G=15 GAMMA=10 LINE=64` |
| S0-3 | `sort ... \| head -n 160000 > ../master_trace_small.csv` | `sort ... > $(TRACE)`，无截断 |
| S0-4 | `opt` 无前置依赖；`generator -i *.csv --binary t*.exe`（glob） | `opt: $(TRACE) $(BIN)`；`-i $(TRACE) --binary $(BIN)`（显式） |
| S0-4 | `all:` 内联 4 行编译 | `all: $(BIN)` → `$(BIN): targ.ll` → `targ.ll: tsin.ll` → `tsin.ll: $(TU_LL)`；`$(TRACE): $(BIN)` |
| S0-4 | `clang++ ... *.cpp -o tsin.ll`（多输入 + 单 `-o`，clang 直接拒绝） | `%.ll: %.cpp` 逐 TU 编译，`llvm-link` 合并 |
| S0-4 | 无 | 多 `main()` 提前拦截：`.ll` 中 `^define .* @main(` 多于一处即报错退出 |
| S0-1 | `sum: python summarize.py`（只打 stdout） | `sum: report`；`report` 落盘 `reports/<tag>.txt` 并 tee |
| S0-5 | 无 | 新增 `verify`（幂等自检，只记录不作门禁） |

## 2. 新增变量

`SRCS`（默认 `$(wildcard *.cpp)`，无硬编码文件名）、`TU_LL`（`$(SRCS:.cpp=.ll)`）、
`BIN`（`t.exe`）、`TRACE`（`trace.csv`）、`K/G/GAMMA/LINE/PARAMS`、`TAG`、
`REPORTDIR`（`reports`）、`REPORT`。

## 3. 参数取值

`1 15 10 64`。`calibrate.cpp` **不接入**——按你的判断其实测不准，不作为默认值来源。

## 4. 明确未改动

- `opt` 首行 `rm -rf *.dot *.txt *.layout.txt` —— 保留。
- `rm *json` —— 保留（未加 `-f`，行为与原样一致）。
- `generator` 调用未加 `--disable-self-weight` —— 与原文一致（阶段 1 的 S1-4 已把其余三处也对齐）。
- 未加任何注释。

## 5. 验证方法

```bash
make                     # 编译
make run                 # 采样 -> trace.csv
make opt                 # 构图 + 三台开方工具
make report              # 落盘 reports/<tag>.txt，同时打到终端
make verify              # 幂等自检
```

功能等价性自查：单 TU 场景下 `llvm-link` 只是重新序列化，pass 看到的模块与旧版一致，因此 `make opt` 的输出应与旧版逐行相同，只有 `1 1 10 64` 变成 `1 15 10 64`。

多 TU 自检：`make SRCS="a.cpp b.cpp"` 应能合并成一个模块；再往目录里放两个含 `main()` 的文件跑 `make`，应在 `llvm-link` 之前报错退出。

### 5.1 本机已完成的验证（Windows 侧，用现成的 make/grep/sh）

- `make -n all`：Makefile 解析通过（无 missing separator），`$(wildcard *.cpp)` 展开为 4 个 `.ll`，依赖链贯通至 `$(BIN)`。
- 守卫四例实测（临时目录，用伪造的 `.ll` 绕过 clang）：
  - 两个文件定义 `main()` → 拦下，列出 `a.ll b.ll`，`Error 1`；
  - 只有一个定义 `main()` → 放行，进入 `llvm-link`；
  - 两个都不含 `main()` → 放行；
  - `SRCS=zzz.cpp` → `No rule to make target 'zzz.ll'`。
- 断言式样 `^define .* @main(` 已由真实 GNU grep 验证：命中 `define dso_local i32 @main() #0 {` 与 `define dso_local noundef i32 @main() #0 {`，不命中 `define dso_local i32 @helper()`。BRE 下 `(` 按字面处理，无需转义。

### 5.2 本机无法验证、需在 Linux 侧确认的一点

本机没有 `clang++` / `llvm-link` / `opt`（只有 make / grep / sh），所以下面这条是**推断，未实测**：

```
clang++ -O0 -g -S -emit-llvm -gdwarf-4 a.cpp b.cpp -o x.ll
```

预期报 `cannot specify -o when generating multiple output files`。请在 Linux 侧跑这一句确认。若你的 clang 版本反而接受多输入，那么"必须引入 `llvm-link`"这一条就可以省掉，改造也能简化。

幂等自检用法：对已采纳布局的源文件重跑一遍再 verify。

```bash
make opt SRCS=test_fields2x.cpp BIN=tx.exe TRACE=trace_opt.csv
make verify
```

期望两个计数归零。**当前不达 0 属已知现象**：dim 语义（S2-1）未修，加上建议本身不幂等。

## 6. 需要你确认的两处副作用

1. **`SRCS` 默认取 `$(wildcard *.cpp)`，不再有硬编码文件名。** 多个 `.cpp` 按"同一程序的多个 TU"处理：逐 TU 编译出 `.ll`，再由 `llvm-link` 合并。若其中多个文件定义了 `main()`（`benchmark/` 有 4 个备选版本、`D:\tmp\2` 有 3 个），构建会在合并前直接报错退出并提示 `make SRCS=xxx.cpp`。拦截点选在 `.ll` 上而不是源码文本上，避免注释或调用里的 `main(` 造成误判。
2. **报告放在 `reports/` 子目录。** 因为 `rm -rf *.txt` 会吃掉本层所有 `.txt`；`rm` 不递归，子目录安全。这是保留 `rm` 的配套，不是额外要求。

## 7. 未做的部分

- `sort -m` 仍假定 `csv/` 下每个 per-core trace 自身已按时间有序。若不成立，改为全量 `sort`。
- 超参数 provenance 在阶段 0 未加，已由阶段 1 的 S1-5 补上。
- 后续阶段（S2 dim 语义、S3 时间窗与归一化等）均未动。

---

# 阶段 1 实施日志（止血）

改动 7 个文件：`summarize.py`、`summarier/pkg_simple/pkg_fields` 三个 `main.rs`、
`benchmark/test_singles.sh`、`benchmark/test_multidim.sh`、`README.md`、`benchmark/Makefile`。

## 1. 改动清单

| ID | 文件 | 改动 |
|---|---|---|
| S1-1 | `summarize.py` `summarize_arrays` | argmax 加拒绝门槛。先取 `positives = {w>0}`；无正值时不再输出任何布局建议：全 0 → "无有效证据"，有负值 → "净亏损且无正收益证据，保持现状"。只有存在正值时才取 max 并给建议。 |
| S1-2 | 三个 Rust 工具 | 边数对账：`k_edges` 数 DOT 里边块数（`^\s*"x" -- "y" [`），`m_edges` 数正则实际读懂的数；`m < k` 报错退出。`k==0` 且节点 >1 只警告。成功时打印 `[edges] <file>: parsed M/K`。 |
| S1-3 | `summarize.py` `summarize_structs` | 删掉 `except Exception: pass`。改为收集 `dwarf_error`（无二进制 / 退出码非 0 / 未生成类型文件 / DWARF 里找不到该结构体 / 找不到 dbg-viewtype 各给具体原因），取不到就打印 ❌ 错误并只列推荐分组，不给对比结论。`import subprocess` 提到文件头（原先函数内 `import tempfile` 是从未使用的死导入，一并删除）。 |
| S1-4 | `test_singles.sh`、`test_multidim.sh`、`README.md:294` | 去掉 `--disable-self-weight`。`benchmark/Makefile` 本来就没传。`generator` 仍支持该参数（未删）。 |
| S1-5 | `benchmark/Makefile` `report` | 报告头加一行 provenance：`src / trace(行数) / k / g / gamma / line`，再输出 `summarize.py` 的结果。不用 `#`，避免与注释混淆。 |

## 2. 为什么 S1-4 是"全部关掉"而不是"全部打开"

自环不进任何布局决策：`summarier` 的判据要求"恰好一维差 1"（自环各维都差 0，恒不满足）；
`package_simple` / `package_fields` 只把自环写进 `w_matrix` 对角线，而合并循环全是 `for j in (i+1)..n`。
唯一消费者是 `summarize.py` 的共享警报。开这个开关 = 关掉全套工具链里唯一的真/伪共享诊断。

## 3. 本机已完成的验证

- `cargo build`：三个 crate 全部编译通过（唯一 warning 是既有的 `unused import: BufRead`）。
- `summarize.py` 语法检查 + 四分支实测（临时目录构造 `graph.*.txt`）：

| 输入 | 输出 |
|---|---|
| `dim0 0.0 / dim1 -5.6`（即 `matrix_2d` 那次） | ⚠️ 无法判定：净亏损、无正收益证据 → 保持现状 |
| `dim0 0.0 / dim1 0.0` | ⚠️ 无法判定：无有效证据 |
| `dim0 5.0 / dim1 -1.0` | 💡 dim0 最高正亲和度 (5.0) → 维持现有 AOS（原行为保留） |
| `dim0 0.0 / dim1 -26.0 / dim2 298.6` | 💡 dim2 最高 (298.6) → 强烈建议转置（原行为保留） |

- S1-3 错误路径实测：目录里有 `.layout.txt` 但无可执行文件 → `❌ 错误: 无法取得原始布局 —— 目录下没有找到可执行文件`。
- S1-2 端到端实测（用刚编译的 `summarier.exe`）：正常 DOT → `[edges] ...: parsed 2/2`，exit 0；把边上的 `label=` 改成 `weight=` → `Error: ... 有 2 条边, 只解析出 0 条`，exit 1。
- `--disable-self-weight` 全仓库仅剩 `generator/src/main.rs`（参数定义与 usage，保留）与本文档。

## 4. 未做 / 遗留

- **S1-1 只加了拒绝门槛，dim 语义仍是反的（S2-1 未修）**：`max_dim != "dim0"` 才建议转置这条映射没动。
  所以 `matrix_2d` 现在从"错误的维持"变成"无法判定"；**正确结论（是该转置）要等 S2-1 + S3-1。**
- `summarier` 的 `unused import: BufRead` 是既有 warning，未顺手清理。
- `test_singles.sh` / `test_multidim.sh` 里仍写死 `1 1 10 64`（与 Makefile 的 `1 15 10 64` 不一致），
  且路径是旧的 `/mnt/hgfs/...`。这两个脚本已被 `benchmark/Makefile` 取代，未一并清理。
- provenance 只在 `benchmark/Makefile` 里加；两个 `.sh` 没有。

## 5. 验收（2026-10-13 实跑）

数据来源：`D:\tmp\2\3`，命令 `make SRCS=test_fields2.cpp run opt report`，
trace 55361 行，`k=1 g=15 gamma=10 line=64`。

| ID | 判定 | 依据 |
|---|---|---|
| S1-1 | 通过 | `matrix_2d`（0.0 / −75.0）→ "⚠️ 无法判定"；`tensor_3d` 1202、`entity_arr` 127.7、`weapon_arr` 72.6 仍走正分通道 |
| S1-2 | 间接通过 | 8 个输出文件（4 个 `.txt` + 4 个 `.layout.txt`）全部生成。检查点在写文件之前，故未误报。但 `[edges] M/K` 只打到 `make opt` 屏幕，**报告里看不到数字**（实现时漏了"写进报告头"） |
| S1-3 | 通过 | 三个结构体都给出了来自 DWARF 的 `[原始代码顺序]`，未出现 ❌ 错误 |
| S1-4 | 通过 | 自环存在（警报响），说明开关确实未传 |
| S1-5 | 通过 | 报告首行为 provenance |

## 6. 实跑暴露的两个问题（归入阶段 4）

**1. 假警报全响，真警报不响。**

- `ComplexEntity` + `WeaponStats` 共 16 个数组字段**全部**报 "🚨 真共享警报 → Thread-Local 线程私有化或原子规约"。
  但数组字段的自环是**跨实例同字段**，正解是 SOA / padding，不是线程私有化。
- 反查 `graph.GameConfig.fields.dot`，6 个字段自环的 `alpha` **全为 0**：

  | 字段 | alpha | beta |
  |---|---|---|
  | difficulty | 0 | 42 |
  | max_retries | 0 | 31 |
  | max_players | 0 | 16 |
  | timeout_ms | 0 | 16 |
  | drop_rate | 0 | 7 |
  | spawn_rate | 0 | 6 |

  而 `global_cfg.difficulty` 是 4 核写**同一地址**，教科书级真共享，**一次都没被记到**。
  对照 `after.txt`（`ITERATIONS=100000` 那版）该字段 α=10。
- 原因：自环要求"相邻两条访问是同一字段、且来自不同核"。本次 trace 只有 55361 行，采样太稀，
  两个核的 difficulty 访问从未在流水里相邻。
- 两类判据**形状完全相同**（都是自环），区别只在"两次访问是否**同一地址**"——该信息在抽象成
  `Struct::field` 时被销毁。→ S4-1 / S4-2。

**2. `alpha > 0` 的边共 108 条**，其中大量形如 `GameConfig::difficulty -- tensor_3d[..][..][..]`
（每条 α=1），即"**跨对象**的前后脚相邻访问"。这是"异行 + 跨核 + 相邻"噪声在整张图上的分布。

---

# 阶段 2 实施日志（判决层）

## 1. dim 语义：工具算的和报告说的不是同一件事

### 1.1 代码事实

`summarier` 的 `hyp_linear_index(indices, bounds, dim_k)`（`main.rs:27-39`）把 `dim_k`
**挪到下标列表末尾**；而 `orig_linear_index`（`main.rs:17-25`）从末尾开始乘、末尾 multiplier = 1，
即**末尾下标变化最快 = 最内层**。

所以「dim_k 得分最高」的准确含义是：**以 dim_k 为最内层的布局最好**。而 C 数组最内层是
**最后一个下标**。

### 1.2 报告层的假设是反的

改前 `summarize.py` 写的是：

```python
if max_dim != "dim0":   # → 建议转置
else:                   # → 维持现有 AOS
```

它把 **dim0 当成"当前布局"**。而当前布局是**最后一维**。于是：

| 数组 | 维度含义 | 最高分 | 改前报告 | 事实 |
|---|---|---|---|---|
| `tensor_3d` | [i, t, k] | dim2 = 1202 | "建议转置，让 dim2 连续" | **dim2 = k 本来就是最内层**，等于叫人搬去已在的地方 |
| `entity_arr` | [i, t, 字段] | dim2 = 127.7 | "建议转置/SOA，让 dim2 连续" | 字段维连续 = **AOS**，就是现状 |
| `matrix_2d` | [i, t] | dim0 = 0（零边） | (S1-1 前)"维持现有 AOS" | dim1 = **−75**，工具自己算出现状在亏 |

**3/3 全错。**

### 1.3 改法（已实施并验证）

`summarize.py` 改为与**最后一维**比较：

```python
innermost = f"dim{len(weights) - 1}"
if max_dim == innermost:  → "维持现有布局即可（该维本来就是最内层）"
else:                     → "强烈建议转置，以便让 max_dim 成为最内层"
```

用 2026-10-13 的真实数据验证：

| 数组 | 改后 |
|---|---|
| `tensor_3d` | **维持现有布局** |
| `entity_arr` | **维持**（与几何独立验证一致，见 §3） |
| `weapon_arr` | **维持** |
| `matrix_2d` | 仍为"无法判定"（dim0 零边，见 §2） |
| 构造用例 dim0=5 / dim1=−1 | **强烈建议转置**（另一支正常） |

## 2. 评分集合：每个维度只统计"自己那类邻居"，会出现空集

`main.rs:263-277` 的筛子要求"只有 dim_k 差 1、其余维全等"。于是每个维度只在**自己那一类边**上求和：

| `matrix_2d` | 统计哪些配对 | 条数 | 分数 |
|---|---|---|---|
| dim1（现状） | 同 i、相邻 t | 5 | **−75** |
| dim0（转置） | 同 t、相邻 i | **0** | **0** |

**5 条打架的配对 vs 0 条（什么都没统计到）→ 0 赢。** dim0 的 0 不是"转置得 0 分"，是**空集**。

实测（`graph.matrix_2d.dot` 共 86 条边 = 64 自环 + 22 非自环，其中该维差 1 的只有 5 条）：

| 布局 | 全部边求和 |
|---|---|
| dim1 最内 = 现状 | 378.5 |
| dim0 最内 = 转置 | 544.6 |
| **增量** | **+166.1** |

**结论：`matrix_2d` 该转置。** 物理上也成立：每行 8 个 long = 64 字节 = 整条 cacheline，
4 个线程各写 8 字节 → 4 路伪共享。

**同一方法算 `tensor_3d`（912 条边）：**

| 布局 | 全部边求和 | 增量 |
|---|---|---|
| dim2 最内 = 现状 | 2971.3 | — |
| dim0 最内（thread 提到最外） | 2381.1 | **−590.2** |
| dim1 最内 | 2362.5 | −608.8 |

**即：按工具自己的模型，`tensor_3d` 该保持现状，转置是净亏。**
物理上可解释：它的行是 4 个 long = 32 字节，只有 **2 个**线程共用一条（4 路 vs 2 路），
而 k 内层循环是 4 个连续元素吃同一条线，转置会把这份内层局部性打散到 4 条线上。

### 2.1 与已落地改动的差异（重要）

| 数组 | 工具当时说的 | 你实际做的 | 修正模型说 |
|---|---|---|---|
| `matrix_2d` | 维持现有 AOS | **没动** | **该转置（＋166.1）** |
| `tensor_3d` | 强烈建议转置 | **转了** | **该保持（−590.2）** |

**最该转的没转，不该转的转了。** 且 0.114→0.095s 是一揽子改动，无法归因。

**§2 的评分改动未实施**：它会让 `tensor_3d` 从"转置"翻成"保持"，需要先单独测量
`matrix_2d` / `tensor_3d` 各自的效果作为裁判。

## 3. 新发现：`bounds` 取自观测下标，导致"现状布局"不是真实布局

`main.rs:158-165`：

```rust
let mut bounds = vec![0; num_dims];
for n in nodes.values() { for (d, &idx) in n.indices.iter().enumerate() {
    if idx + 1 > bounds[d] { bounds[d] = idx + 1; } } }
```

它取**观测到的下标最大值 + 1**，不是声明的维度。`matrix_2d[16][8]` 但只有 4 个线程跑
→ `t∈[0,4)` → **bounds = [16,4]**，工具按线程步长 4 元素（32 字节）建模；
**真实步长是 8 元素（64 字节）**。

所以工具算的"现状"根本不是程序的现状，转置增量的**绝对值**不可信（方向不受影响：
按声明维度 [16,8] 算是 +105.7，按观测 [16,4] 算是 +166.1，都为正）。

正解：让 `generator` 把 DWARF 里的声明维度写进 DOT（`int[16][8]` 本来就在类型里），
不要在下游从观测下标回推。

`entity_arr` 的独立几何验证（来自 DOT 里的 `offset=`，与 bounds 无关）：
health 0/320/2560/2880、mana 64/384/2624/2944 → stride 320 = 5×64，**每个字段都落在 64 字节边界**
→ 线程之间不可能共享 cacheline → **正确答案是"保持现状"**，与 §1.3 改后输出一致。

## 4. 本文档的勘误

- 早先给出的 `matrix_2d` 增量为 **+105.7**、`tensor_3d` 各布局为 2381/1992/2971 —— 用的是
  **错 bounds [16,8,4]**。修正为 +166.1 与 2381/2362/2971（`tensor_3d` 的 dim0/dim2 恰好与
  bounds 无关，故数值未变）。
- 复现口径：`bounds` 用观测值 [16,4] / [16,4,4] 时，可精确复现工具的 `0 / −75` 与
  `0 / −206 / 1202`。这是本次所有推算的校验依据。
