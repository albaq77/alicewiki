# 当前固定真实基准

来源：2026-10-16 对 `test_fields2.cpp` 重新构建并插桩后的 review 运行。

- `trace.csv.gz`：55379 行六列 Trace，gzip mtime=0；解压 SHA256 为
  `6f44040433bc6d4c4240121ad2b60685fec02303e2275e16ba48ed4d6f8119b6`。
- `types.txt` / `vars.txt`：同一 ELF 的 dbg-viewtype 原始转储，保留类型和地址映射。
- `expected.json`：输入/源文件哈希、window=30000ns 等参数、全部图/布局摘要、47 个候选和最终判决。

预期来自完成解析修复后的流水线，并通过独立 Python 公式和几何核验。
原始共享行数为 matrix_2d=16、tensor_3d=16、entity_arr=0、weapon_arr=0；后两者的 SOA 推荐是模型亲和收益。

执行 `python3 test/replay_baseline.py` 只验证，不刷新预期。
本夹具明确替代缺失原始输入的 55361 行历史基准；旧数值仍保留在 `test/baseline.json`，不冒充旧输入重放。
