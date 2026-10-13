import os
import sys
import glob
import re

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
        with open(filepath, 'r', encoding='utf-8') as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) == 2 and parts[0].startswith("dim"):
                    weights[parts[0]] = float(parts[1])

        if not weights:
            continue

        print(f"🔹 数组实体: {color_text(array_name, '92')}")
        for dim, w in weights.items():
            print(f"   - {dim}: 亲和度权重 = {w}")

        if all(w == 0 for w in weights.values()):
            print("   💡 建议: 所有维度权重为 0。说明循环中穿插了其他读写（如全局变量），打断了该数组的连续局部性，建议先消除干扰。")
        else:
            max_dim = max(weights, key=weights.get)
            print(f"   💡 诊断: {max_dim} 维度具有极高的访存亲和度。")
            if max_dim != "dim0":
                print(f"      👉 强烈建议【转置 (Transpose) / 转为 SOA 架构】，以便让 {max_dim} 维度在物理内存中连续存放！")
            else:
                print(f"      👉 维持现有 AOS 布局即可，当前主干维度已有最优的 Cache 命中率。")
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
        exe_files = glob.glob(os.path.join(data_dir, "*.exe")) + glob.glob(os.path.join(data_dir, "*.out")) + glob.glob(os.path.join(data_dir, "t*"))
        binary = exe_files[0] if exe_files else None
        
        if binary:
            import subprocess
            import tempfile
            t_path = os.path.join(data_dir, "tmp_type.out")
            v_path = os.path.join(data_dir, "tmp_var.out")
            try:
                subprocess.run(["dbg-viewtype", binary, t_path, v_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if os.path.exists(t_path):
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
            except Exception:
                pass
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
            print("   (未找到对应的 DWARF 符号信息，无法精准推断原始布局，请确保目录下有可执行文件且安装了 dbg-viewtype)")
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
