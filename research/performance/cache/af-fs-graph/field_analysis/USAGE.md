


The project copied from https://github.com/albaq77/FieldAnalysis.

And some changes are not logged by git.


### 第一步：编译采样器本身
将您提取出来的精简版 `FieldAnalysis-master` 传到您的 Linux/LLVM 开发机上，执行以下命令：
```bash
cd FieldAnalysis-master
mkdir build && cd build

# 这里的 LLVM_DIR 请替换为您系统里的实际 LLVM CMake 路径
# 如果 LLVM 已经全局安装，直接 cmake .. 即可
cmake .. -DLLVM_DIR=/usr/lib/llvm-14/lib/cmake/llvm
make -j4
```
编译成功后，您手中就拿到了两件核心武器：
1. **插桩手术刀**：`build/FieldAnalysisPass.so` (LLVM 插件)
2. **运行记录仪**：`build/runtime/libaffinity.a` (静态库)

---

### 第二步：对目标程序进行插桩与采样
假设您要测试的目标程序叫 `app.cpp`。

为了百分百确保插桩生效，最稳妥的做法是分步进行（先转 IR，再插桩，最后生成可执行文件）：

**1. 将源码编译为 LLVM IR (文本格式)**
```bash
clang++ -O0 -g -S -emit-llvm app.cpp -o app.ll
```

**2. 使用 `opt` 注入采样探针**
调用刚刚编译好的 `.so` 插件，在 IR 里打下追踪标记：
```bash
opt -load-pass-plugin=/path/to/build/FieldAnalysisPass.so -passes="field-analysis" app.ll -S -o app_inst.ll
```

**3. 链接运行时，生成最终可执行文件**
把带着探针的 IR 和您的“运行记录仪”缝合在一起：
```bash
clang++ app_inst.ll /path/to/build/runtime/libaffinity.a -lpthread -o app
```

---

### 第三步：运行程序与查看 CSV

现在您的 `app` 已经是一个“全天候内存监控版”的程序了。您可以直接运行它，也可以通过环境变量来调节它的采样行为：

```bash
# 默认全量抓取，并在当前目录下生成 access_trace_*.csv
./app

# 【高阶玩法】环境变量控制：
# FA_TRACE_DIR: 指定生成的 CSV 存在哪个文件夹
# TRACE_SAMPLE_RATE: 设置采样率 (比如设为 100，就是每 100 次访存抓 1 次，降低性能损耗)
# TRACE_FLUSH_THRESHOLD: 缓存行大小，默认 4096 条刷一次盘
FA_TRACE_DIR=/tmp/my_traces TRACE_SAMPLE_RATE=10 ./app
```

程序运行结束后，您在目录里就能看到生成的 `access_trace_*.csv`。
直接用 `cat` 或 `head` 查看，里面就是完全适配您 Rust `generator` 期望的纯净格式：
```csv
164324234567,0x408200,W,8,2
```
然后您就可以拿着这个文件，直接喂给您的图计算工具去跑了！