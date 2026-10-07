import os
import sys
import glob
import re

def color_text(text, color_code):
    # 93 is Bright Yellow, 92 is Bright Green
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


def find_struct_layout_in_cpp(struct_name, search_dir):
    import re
    # Simple regex to find struct body
    struct_re = re.compile(rf'struct\\s+{struct_name}\\s*\\{{([^}}]+)\}}', re.MULTILINE)
    
    # Try to find cpp/h/hpp files
    src_files = glob.glob(os.path.join(search_dir, "*.cpp")) +                 glob.glob(os.path.join(search_dir, "*.h")) +                 glob.glob(os.path.join(search_dir, "*.hpp")) +                 glob.glob(os.path.join(search_dir, "*.c"))
                
    for filepath in src_files:
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                content = f.read()
            m = struct_re.search(content)
            if m:
                body = m.group(1)
                # extract variable names
                # e.g., "int health; float score;"
                fields = []
                lines = body.split(';')
                for line in lines:
                    line = line.strip()
                    if not line or line.startswith('//'):
                        continue
                    # get the last word before ; as the field name
                    parts = line.split()
                    if parts:
                        name = parts[-1].replace('*', '').replace(';', '')
                        # handle arrays like arr[10]
                        name = name.split('[')[0]
                        if name:
                            fields.append(name)
                return fields
        except:
            pass
    return []

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

        # 2. Parse Original Layout from C++ source files
        # We assume the source files are in the parent directory of csv/ or in the data_dir itself
        search_dirs = [data_dir, os.path.dirname(data_dir), os.path.join(os.path.dirname(data_dir), "..")]
        original = []
        for d in search_dirs:
            original = find_struct_layout_in_cpp(struct_name, d)
            if original:
                break
        
        # Filter original fields to only those that actually appear in the recommended layout 
        # (since dynamic tracing only shows accessed fields)
        if original:
            original = [f for f in original if f in flat_recommended]

        print(f"🔹 结构体: {color_text(struct_name, '92')}")
        # Fallback to DOT offsets if C++ parsing fails
        if not original:
            dot_file = os.path.join(data_dir, f"graph.{struct_name}.fields.dot")
            if os.path.exists(dot_file):
                import re
                node_re = re.compile(rf'^\s*"{struct_name}::([^"]+)"\s*\[.*offset=(\d+).*\]')
                nodes = []
                with open(dot_file, 'r', encoding='utf-8') as f:
                    for line in f:
                        m = node_re.search(line)
                        if m:
                            nodes.append((m.group(1), int(m.group(2))))
                nodes.sort(key=lambda x: x[1])
                original = [n[0] for n in nodes]
                original = [f for f in original if f in flat_recommended]
        
        if not original:
            print("   (未找到 C++ 源码或 DOT 文件，无法推断原始布局)")
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

    if not lines or len(lines) <= 3:
        print("  无离散全局变量需要打包。\n")
        return

    print("🔹 全局变量亲和性分组:")
    for line in lines:
        line = line.strip()
        if not line or line.startswith("Target:") or line.startswith("Cacheline size:") or line.startswith("==="):
            continue
        print(f"   {line}")
    print("\n   💡 建议: 将属于同一 Cacheline 的全局变量在 C/C++ 中紧凑定义，或利用 __attribute__((aligned(64))) 将它们对齐在一块。")
    print()

if __name__ == "__main__":
    import platform
    if platform.system() == "Windows":
        os.system("color") # Enable ANSI escape codes in Windows cmd

    target_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(target_dir):
        print(f"错误: 目录 '{target_dir}' 不存在。")
        sys.exit(1)
        
    print(f"\n🚀 正在分析数据目录: {target_dir}\n")
    summarize_arrays(target_dir)
    summarize_structs(target_dir)
    summarize_globals(target_dir)
