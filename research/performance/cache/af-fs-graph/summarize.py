import os
import sys
import glob
import re
import subprocess

def color_text(text, color_code):
    return f"\033[{color_code}m{text}\033[0m"

def summarize_arrays(data_dir):
    print("="*60)
    print("📊 多维数组优化建议 (Array Layout & Transpose)")
    print("="*60)
    txt_files = glob.glob(os.path.join(data_dir, "graph.*.txt"))
    array_files = [f for f in txt_files if not f.endswith(".layout.txt")]

    if not array_files:
        print("  未检测到数组分析结果。\n")
        return

    for filepath in array_files:
        array_name = os.path.basename(filepath).replace("graph.", "").replace(".txt", "")
        weights = {}
        pairs = {}
        sizes = {}
        kinds = {}
        with open(filepath, 'r', encoding='utf-8') as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 3 and parts[0].startswith("dim"):
                    weights[parts[0]] = float(parts[1])
                    pairs[parts[0]] = int(parts[2])
                    for tok in parts[3:]:
                        if tok.startswith("size="):
                            sizes[parts[0]] = tok[5:]
                        elif tok == "field":
                            kinds[parts[0]] = "field"

        if not weights:
            print(f"🔹 数组实体: {color_text(array_name, '92')}")
            print(f"   ❌ 无法解析 {os.path.basename(filepath)}: 期望 'dim<k> <平均权重> <配对数> [size=<n>] [field]'。")
            print()
            continue

        print(f"🔹 数组实体: {color_text(array_name, '92')}")
        for dim, w in weights.items():
            tag = "字段维" if kinds.get(dim) == "field" else "下标维"
            print(f"   - {dim}[{tag}, 尺寸={sizes.get(dim, '?')}]: 平均权重 = {w}  (配对数 = {pairs.get(dim, 0)})")

        usable = {d: w for d, w in weights.items() if pairs.get(d, 0) > 0}
        if not usable:
            print("   ⚠️ 无法判定: 所有维度的配对数都是 0（窗口内没有观测到任何关系）。")
            print()
            continue

        positives = {d: w for d, w in usable.items() if w > 0}
        if not positives:
            neg = ", ".join(f"{d}={w}" for d, w in usable.items() if w < 0)
            print("   ⚠️ 无法判定: 没有任何维度提供正收益证据。")
            if neg:
                print(f"      负值: {neg}")
            else:
                print("      有配对数的维度平均权重均为 0。")
            print("      👉 建议: 保持现状，先排查采样/数据链路，不要据此改动布局。")
            print()
            continue

        max_dim = max(positives, key=positives.get)
        innermost = f"dim{len(weights) - 1}"
        print(f"   💡 诊断: {max_dim} 维度具有最高的平均权重 ({positives[max_dim]})。")
        if max_dim == innermost:
            print(f"      👉 维持现有布局即可: {max_dim} 本来就是最内层（物理上连续的那一维）。")
        else:
            print(f"      👉 强烈建议【转置 (Transpose) / 转为 SOA 架构】，以便让 {max_dim} 维度成为最内层！")
        print()

def summarize_structs(data_dir):
    print("="*60)
    print("📦 结构体内存布局重排建议 (Struct Cacheline Packing)")
    print("="*60)
    layout_files = glob.glob(os.path.join(data_dir, "graph.*.layout.txt"))

    for filepath in layout_files:
        basename = os.path.basename(filepath)
        if basename == "graph.layout.txt":
            continue

        struct_name = basename.replace("graph.", "").replace(".layout.txt", "")

        # 1. Parse Recommended Layout
        recommended_groups = []
        current_group = []
        with open(filepath, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if line.startswith("[Cacheline"):
                    if current_group:
                        recommended_groups.append(current_group)
                    current_group = []
                elif line.startswith("- "):
                    current_group.append(line[2:])
        if current_group:
            recommended_groups.append(current_group)
            
        flat_recommended = [f for g in recommended_groups for f in g]

        # 2. 调用 dbg-viewtype 直接从二进制提取原生 DWARF 符号偏移
        original = []
        dwarf_error = None
        exe_files = glob.glob(os.path.join(data_dir, "*.exe")) + glob.glob(os.path.join(data_dir, "*.out")) + glob.glob(os.path.join(data_dir, "t*"))
        binary = exe_files[0] if exe_files else None

        if binary is None:
            dwarf_error = "目录下没有找到可执行文件 (.exe/.out/t*)"
        else:
            t_path = os.path.join(data_dir, "tmp_type.out")
            v_path = os.path.join(data_dir, "tmp_var.out")
            try:
                proc = subprocess.run(["dbg-viewtype", binary, t_path, v_path], capture_output=True, text=True)
                if proc.returncode != 0:
                    dwarf_error = f"dbg-viewtype 退出码 {proc.returncode}: {(proc.stderr or '').strip()[:200]}"
                elif not os.path.exists(t_path):
                    dwarf_error = f"dbg-viewtype 未生成类型文件 {t_path}"
                else:
                    parsing_target = False
                    nodes = []
                    with open(t_path, 'r', encoding='utf-8') as tf:
                        for line in tf:
                            if line.startswith('\t'):
                                if parsing_target:
                                    parts = line.strip().split('\t')
                                    if len(parts) >= 3:
                                        offset = int(parts[0])
                                        name = parts[1]
                                        nodes.append((name, offset))
                            else:
                                line_trim = line.strip()
                                if line_trim:
                                    sname = line_trim.split('\t')[0]
                                    if sname == struct_name:
                                        parsing_target = True
                                    else:
                                        parsing_target = False
                    nodes.sort(key=lambda x: x[1])
                    original = [n[0] for n in nodes]
                    if not original:
                        dwarf_error = f"DWARF 中未找到结构体 '{struct_name}' 的成员（名字不匹配？）"
            except FileNotFoundError:
                dwarf_error = "找不到 dbg-viewtype 可执行文件（不在 PATH 中）"
            except Exception as e:
                dwarf_error = f"调用 dbg-viewtype 失败: {e}"
            finally:
                if os.path.exists(t_path): os.remove(t_path)
                if os.path.exists(v_path): os.remove(v_path)
                
        # 不再过滤原始字段！完整保留 DWARF 中的所有字段（包含如 mana 这样未被访问的冷字段）
        # 以便向用户展示最真实的内存布局对比
        # 解析自环边检测真共享 (True Sharing)
        severe_ts_fields = []
        dot_file = os.path.join(data_dir, f"graph.{struct_name}.fields.dot")
        if os.path.exists(dot_file):
            with open(dot_file, 'r', encoding='utf-8') as f:
                content = f.read()
            pattern = re.compile(rf'"(?:[^:]+::)?([^"]+)"\s*--\s*"(?:[^:]+::)?\1"\s*\[(.*?)\]', re.DOTALL)
            for match in pattern.finditer(content):
                field_name = match.group(1)
                attrs = match.group(2)
                alpha_match = re.search(r'alpha=(\d+)', attrs)
                if alpha_match:
                    alpha_val = int(alpha_match.group(1))
                    if alpha_val > 0:
                        severe_ts_fields.append((field_name, alpha_val))

        print(f"🔹 结构体: {color_text(struct_name, '92')}")
        
        if severe_ts_fields:
            print(f"   🚨 {color_text('真共享警报', '91')}: 侦测到跨核真共享冲突 (True Sharing)！")
            for ts_field, alpha_val in severe_ts_fields:
                print(f"      - 高频竞争字段: {color_text(ts_field, '91')} (跨核真共享冲突强度 Alpha={alpha_val})")
            print(f"   👉 {color_text('优化建议', '93')}: 该字段为多线程高频并发读写热点，建议采用 Thread-Local 线程私有化或原子规约 (Reduce) 降低同步开销。\n")
        if not original:
            print(f"   ❌ 错误: 无法取得原始布局 —— {dwarf_error}")
            print("      DWARF 是原始字段顺序的唯一真值来源，缺少它无法做布局对比。")
            print("      下面只列出推荐分组，不给出对比结论：")
            for i, group in enumerate(recommended_groups):
                print(f"      [Cacheline {i}]: " + " | ".join(group))
        else:
            if original == flat_recommended:
                print("   ✅ 诊断: 当前代码中的成员定义顺序已是最优，无需修改。")
                print("   [维持现有布局]: " + " -> ".join(original))
            else:
                print("   ⚠️ 诊断: 发现更优布局！建议根据高亮成员重排 C/C++ 代码：")
                print("   [原始代码顺序]: " + " -> ".join(original))
                print("   [推荐物理布局]: ")
                
                for i, group in enumerate(recommended_groups):
                    colored_group = []
                    for field in group:
                        orig_idx = original.index(field) if field in original else -1
                        rec_idx = flat_recommended.index(field)
                        if orig_idx == rec_idx:
                            colored_group.append(field) # 没动位置
                        else:
                            colored_group.append(color_text(f"{field}(*)", "93")) # 移动了位置，标黄+星号
                    print(f"      📦 Cacheline {i}: " + " | ".join(colored_group))
        print()

def summarize_globals(data_dir):
    print("="*60)
    print("🌍 离散全局变量打包建议 (Global Variables Alignment)")
    print("="*60)
    global_file = os.path.join(data_dir, "graph.layout.txt")
    if not os.path.exists(global_file):
        print("  未检测到全局变量布局结果。\n")
        return

    with open(global_file, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    valid_lines = [l.strip() for l in lines if l.strip()]

    if not valid_lines:
        print("  无离散全局变量需要打包。\n")
        return

    print("🔹 全局变量亲和性分组:")
    for i, line in enumerate(valid_lines):
        print(f"   📦 Cacheline {i}: {line}")
    print("\n   💡 建议: 将属于同一 Cacheline 的全局变量在 C/C++ 中紧凑定义，或利用 __attribute__((aligned(64))) 将它们对齐在一块。")
    print()

if __name__ == "__main__":
    import platform
    if platform.system() == "Windows":
        os.system("color") 

    target_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(target_dir):
        print(f"错误: 目录 '{target_dir}' 不存在。")
        sys.exit(1)
        
    print(f"\n🚀 正在分析数据目录: {target_dir}\n")
    summarize_arrays(target_dir)
    summarize_structs(target_dir)
    summarize_globals(target_dir)
