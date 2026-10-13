# DEVELOP1.md — 阶段0 实施日志

对应 `benchmark/Makefile`。改动文件只有一个。

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
- `generator` 调用未加 `--disable-self-weight` —— 与原文一致；三处驱动不统一的问题仍属 S1-4。
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
- 超参数 provenance（把实际用的 k/g/gamma/line 写进报告头）**未加**，按你不加多余东西的要求省去。需要时是一行 `echo`。
- 后续阶段（S1-1 判决拒绝门槛、S1-2/S1-3 静默失败改报错、S1-4 开关统一、S2 dim 语义）均未动。
