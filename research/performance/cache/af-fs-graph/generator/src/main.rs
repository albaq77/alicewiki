use std::collections::{BTreeMap, HashMap};
use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::process::{Command, exit};

#[derive(Debug)]
struct Config {
    input: String,
    output: String,
    binary: String,
    cache_line_size: u64,
    dbg_viewtype: String,
    disable_self_weight: bool,
}

fn print_usage_and_exit(prog_name: &str) -> ! {
    eprintln!(
        "Usage: {} -i <trace.csv> -o <graph.dot> --binary <app> [--cache-line-size <size>] [--dbg-viewtype <path>] [--disable-self-weight]",
        prog_name
    );
    exit(1);
}

fn parse_args() -> Config {
    let args: Vec<String> = env::args().collect();
    let prog_name = args.get(0).cloned().unwrap_or_else(|| "generator".to_string());

    let mut input = None;
    let mut output = None;
    let mut binary = None;
    let mut cache_line_size = 64;
    let mut dbg_viewtype = "dbg-viewtype".to_string();
    let mut disable_self_weight = false;

    let mut i = 1;
    while i < args.len() {
        let arg = &args[i];
        if arg == "-i" || arg == "--input" {
            if i + 1 < args.len() {
                input = Some(args[i + 1].clone());
                i += 1;
            } else {
                print_usage_and_exit(&prog_name);
            }
        } else if arg == "-o" || arg == "--output" {
            if i + 1 < args.len() {
                output = Some(args[i + 1].clone());
                i += 1;
            } else {
                print_usage_and_exit(&prog_name);
            }
        } else if arg == "--binary" {
            if i + 1 < args.len() {
                binary = Some(args[i + 1].clone());
                i += 1;
            } else {
                print_usage_and_exit(&prog_name);
            }
        } else if arg == "--cache-line-size" {
            if i + 1 < args.len() {
                cache_line_size = args[i + 1].parse().unwrap_or(0);
                if cache_line_size == 0 {
                    eprintln!("Error: --cache-line-size cannot be 0.");
                    exit(1);
                }
                i += 1;
            } else {
                print_usage_and_exit(&prog_name);
            }
        } else if arg == "--dbg-viewtype" {
            if i + 1 < args.len() {
                dbg_viewtype = args[i + 1].clone();
                i += 1;
            } else {
                print_usage_and_exit(&prog_name);
            }
        } else if arg == "--disable-self-weight" {
            disable_self_weight = true;
        } else {
            eprintln!("Unknown argument: {}", arg);
            print_usage_and_exit(&prog_name);
        }
        i += 1;
    }

    let input = input.unwrap_or_else(|| {
        eprintln!("Error: Missing required argument -i/--input");
        print_usage_and_exit(&prog_name);
    });
    let output = output.unwrap_or_else(|| {
        eprintln!("Error: Missing required argument -o/--output");
        print_usage_and_exit(&prog_name);
    });
    let binary = binary.unwrap_or_else(|| {
        eprintln!("Error: Missing required argument --binary");
        print_usage_and_exit(&prog_name);
    });

    Config {
        input,
        output,
        binary,
        cache_line_size,
        dbg_viewtype,
        disable_self_weight,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Rw {
    Read,
    Write,
}

#[derive(Debug, Clone)]
struct Access {
    addr: u64,
    rw: Rw,
    size: u64,
    core_id: u32,
}

impl Access {
    fn parse(line: &str) -> Option<Self> {
        let parts: Vec<&str> = line.split(',').map(|s| s.trim()).collect();
        if parts.len() < 6 {
            eprintln!("Warning: invalid line format, skipping: {}", line);
            return None;
        }

        let addr_str = parts[1];
        let addr = if addr_str.starts_with("0x") || addr_str.starts_with("0X") {
            u64::from_str_radix(&addr_str[2..], 16).ok()
        } else {
            addr_str.parse::<u64>().ok()
        };
        let addr = match addr {
            Some(a) => a,
            None => {
                eprintln!("Warning: invalid address, skipping: {}", line);
                return None;
            }
        };

        let rw = match parts[2].to_uppercase().as_str() {
            "R" => Rw::Read,
            "W" => Rw::Write,
            _ => {
                eprintln!("Warning: invalid R/W type, skipping: {}", line);
                return None;
            }
        };

        let size = match parts[3].parse::<u64>() {
            Ok(s) => s,
            Err(_) => {
                eprintln!("Warning: invalid size, skipping: {}", line);
                return None;
            }
        };

        let core_id = match parts[4].parse::<u32>() {
            Ok(c) => c,
            Err(_) => {
                eprintln!("Warning: invalid core_id, skipping: {}", line);
                return None;
            }
        };

        Some(Access {
            addr,
            rw,
            size,
            core_id,
        })
    }
}

#[derive(Debug, Clone, Hash, PartialEq, Eq)]
struct NodeInfo {
    label: String,
    typ: String,
    offset: i64,
}

#[derive(Debug, Default)]
struct EdgeStat {
    count1: u64,
    count2: u64,
    count3: u64,
    count4: u64,
    addr1: u64,
    addr2: u64,
}

#[derive(Debug, Clone)]
struct StructMember {
    offset: u64,
    name: String,
    typ: String,
}

#[derive(Debug, Clone)]
struct StructType {
    name: String,
    size: u64,
    members: BTreeMap<u64, StructMember>,
}

#[derive(Debug, Clone)]
struct GlobalVar {
    name: String,
    addr: u64,
    typ: String,
}

fn parse_types(path: &str) -> HashMap<String, StructType> {
    let mut map = HashMap::new();
    if let Ok(file) = File::open(path) {
        let reader = BufReader::new(file);
        let mut current_struct: Option<StructType> = None;

        for line in reader.lines().flatten() {
            if line.starts_with('\t') {
                if let Some(ref mut st) = current_struct {
                    let parts: Vec<&str> = line.trim_start().split('\t').collect();
                    if parts.len() >= 3 {
                        let offset: u64 = parts[0].parse().unwrap_or(0);
                        let name = parts[1].to_string();
                        let typ = parts[2].trim_matches('\'').to_string();
                        st.members.insert(offset, StructMember { offset, name, typ });
                    }
                }
            } else {
                if let Some(st) = current_struct.take() {
                    map.insert(st.name.clone(), st);
                }
                let line_trim = line.trim();
                if !line_trim.is_empty() {
                    let mut parts = line_trim.split('\t');
                    let struct_name = parts.next().unwrap_or("").to_string();
                    let size: u64 = parts.next().and_then(|s| s.parse().ok()).unwrap_or(0);
                    current_struct = Some(StructType {
                        name: struct_name,
                        size,
                        members: BTreeMap::new(),
                    });
                }
            }
        }
        if let Some(st) = current_struct {
            map.insert(st.name.clone(), st);
        }
    }
    map
}

fn parse_vars(path: &str) -> BTreeMap<u64, GlobalVar> {
    let mut map = BTreeMap::new();
    if let Ok(file) = File::open(path) {
        let reader = BufReader::new(file);
        for line in reader.lines().flatten() {
            let parts: Vec<&str> = line.split('\t').collect();
            if parts.len() >= 3 {
                let name = parts[0].to_string();
                let addr: u64 = parts[1].parse().unwrap_or(0);
                let typ = parts[2].trim_matches('\'').to_string();
                map.insert(addr, GlobalVar { name, addr, typ });
            }
        }
    }
    map
}

fn guess_base_size(type_str: &str) -> Option<u64> {
    let t = type_str.replace("volatile", "").replace("const", "").replace(" ", "");
    match t.as_str() {
        "char" | "signedchar" | "unsignedchar" | "int8_t" | "uint8_t" => Some(1),
        "short" | "unsignedshort" | "int16_t" | "uint16_t" => Some(2),
        "int" | "unsignedint" | "int32_t" | "uint32_t" | "float" => Some(4),
        "long" | "unsignedlong" | "longlong" | "unsignedlonglong" | "longint" | "unsignedlongint" | "int64_t" | "uint64_t" | "double" => Some(8),
        _ if t.ends_with('*') => Some(8),
        _ => None,
    }
}

fn resolve_access(
    addr: u64,
    vars: &BTreeMap<u64, GlobalVar>,
    types: &HashMap<String, StructType>,
) -> Option<NodeInfo> {
    if let Some((&var_addr, var)) = vars.range(..=addr).next_back() {
        if addr - var_addr < 1024 * 1024 * 10 { // 10MB sanity check
            let delta = addr - var_addr;
            let mut label = var.name.clone();
            let mut typ = var.typ.clone();
            let mut final_offset = delta;

            if let Some(st) = types.get(&var.typ) {
                if let Some((&m_offset, m)) = st.members.range(..=delta).next_back() {
                    let mut member_delta = delta - m_offset;
                    label = format!("{}.{}", label, m.name);
                    typ = m.typ.clone();
                    final_offset = member_delta;

                    // Parse all array bounds: e.g. "int[10][20]" -> bounds = [10, 20]
                    let mut bounds = Vec::new();
                    let mut current_str = typ.as_str();
                    while current_str.ends_with(']') {
                        if let Some(start) = current_str.rfind('[') {
                            let bound_str = &current_str[start + 1..current_str.len() - 1];
                            if let Ok(b) = bound_str.parse::<u64>() {
                                bounds.push(b);
                                current_str = &current_str[..start];
                            } else if bound_str.is_empty() {
                                bounds.push(0);
                                current_str = &current_str[..start];
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                    }
                    bounds.reverse();

                    if !bounds.is_empty() {
                        let total_elements: u64 = bounds.iter().map(|&x| std::cmp::max(x, 1)).product();
                        
                        let mut total_size = 0;
                        if let Some((&next_offset, _)) = st.members.range(m_offset + 1..).next() {
                            total_size = next_offset - m_offset;
                        }

                        let mut elem_size = 0;
                        if total_size > 0 && total_elements > 0 {
                            elem_size = total_size / total_elements;
                        }
                        if elem_size == 0 {
                            if let Some(guess) = guess_base_size(current_str) {
                                elem_size = guess;
                            } else if let Some(struct_info) = types.get(current_str) {
                                elem_size = struct_info.size;
                            }
                        }

                        if elem_size > 0 {
                            let mut linear_index = member_delta / elem_size;
                            member_delta = member_delta % elem_size;

                            let mut indices = Vec::new();
                            for &bound in bounds.iter().rev() {
                                let dim_bound = std::cmp::max(bound, 1);
                                indices.push(linear_index % dim_bound);
                                linear_index /= dim_bound;
                            }
                            indices.reverse();

                            for idx in indices {
                                label = format!("{}[{}]", label, idx);
                            }
                        } else {
                            if member_delta == 0 {
                                for _ in 0..bounds.len() {
                                    label = format!("{}[0]", label);
                                }
                            } else {
                                label = format!("{}[?]+{}", label, member_delta);
                                member_delta = 0;
                            }
                        }
                    }

                    if member_delta > 0 {
                        label = format!("{}+{}", label, member_delta);
                    }
                }
            } else {
                // Global variable is an array itself?
                let mut bounds = Vec::new();
                let mut current_str = typ.as_str();
                while current_str.ends_with(']') {
                    if let Some(start) = current_str.rfind('[') {
                        let bound_str = &current_str[start + 1..current_str.len() - 1];
                        if let Ok(b) = bound_str.parse::<u64>() {
                            bounds.push(b);
                            current_str = &current_str[..start];
                        } else if bound_str.is_empty() {
                            bounds.push(0);
                            current_str = &current_str[..start];
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                bounds.reverse();

                if !bounds.is_empty() {
                    let total_elements: u64 = bounds.iter().map(|&x| std::cmp::max(x, 1)).product();
                    let mut elem_size = 0;
                    if let Some(guess) = guess_base_size(current_str) {
                        elem_size = guess;
                    } else if let Some(struct_info) = types.get(current_str) {
                        elem_size = struct_info.size;
                    }
                    
                    let mut var_delta = delta;
                    if elem_size > 0 {
                        let mut linear_index = var_delta / elem_size;
                        var_delta = var_delta % elem_size;

                        let mut indices = Vec::new();
                        for &bound in bounds.iter().rev() {
                            let dim_bound = std::cmp::max(bound, 1);
                            indices.push(linear_index % dim_bound);
                            linear_index /= dim_bound;
                        }
                        indices.reverse();

                        for idx in indices {
                            label = format!("{}[{}]", label, idx);
                        }

                        // Always try to resolve struct member even if var_delta is 0
                        if let Some(st) = types.get(current_str) {
                            if let Some((&m_offset, m)) = st.members.range(..=var_delta).next_back() {
                                let member_delta = var_delta - m_offset;
                                label = format!("{}.{}", label, m.name);
                                
                                if member_delta > 0 {
                                    label = format!("{}+{}", label, member_delta);
                                }
                                var_delta = 0;
                            }
                        }
                    } else {
                        if var_delta == 0 {
                            for _ in 0..bounds.len() {
                                label = format!("{}[0]", label);
                            }
                        } else {
                            label = format!("{}[?]+{}", label, var_delta);
                            var_delta = 0;
                        }
                    }
                    if var_delta > 0 {
                        label = format!("{}+{}", label, var_delta);
                    }
                } else if delta > 0 {
                    label = format!("{}+{}", label, delta);
                }
            }

            return Some(NodeInfo {
                label,
                typ,
                offset: final_offset as i64,
            });
        }
    }
    None
}

fn escape_dot_string(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

fn main() {
    let config = parse_args();

    let temp_dir = std::env::temp_dir();
    let type_out_path = temp_dir.join(format!("dbg_type_{}.out", std::process::id()));
    let var_out_path = temp_dir.join(format!("dbg_var_{}.out", std::process::id()));
    let type_out = type_out_path.to_str().unwrap();
    let var_out = var_out_path.to_str().unwrap();

    println!("Running dbg-viewtype to dump DWARF info...");
    let status = Command::new(&config.dbg_viewtype)
        .arg(&config.binary)
        .arg(type_out)
        .arg(var_out)
        .status();

    match status {
        Ok(s) if s.success() => {
            println!("dbg-viewtype finished successfully.");
        }
        Ok(s) => {
            eprintln!("Warning: dbg-viewtype exited with status {}", s);
        }
        Err(e) => {
            eprintln!("Warning: failed to execute dbg-viewtype: {}", e);
        }
    }

    let types_map = parse_types(type_out);
    let vars_map = parse_vars(var_out);

    // Clean up the temporary files so they don't pollute the repository
    let _ = std::fs::remove_file(type_out);
    let _ = std::fs::remove_file(var_out);

    let file = File::open(&config.input).unwrap_or_else(|e| {
        eprintln!("Error opening input file: {}", e);
        exit(1);
    });
    let reader = BufReader::new(file);

    let mut node_map: HashMap<String, NodeInfo> = HashMap::new();
    let mut edge_map: HashMap<(String, String), EdgeStat> = HashMap::new();

    let mut prev_valid: Option<(Access, NodeInfo)> = None;
    let mut warn_limit = 0;

    for line in reader.lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => continue,
        };
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }

        if let Some(curr_acc) = Access::parse(line) {
            if let Some(curr_info) = resolve_access(curr_acc.addr, &vars_map, &types_map) {
                node_map
                    .entry(curr_info.label.clone())
                    .or_insert_with(|| curr_info.clone());

                if let Some((prev_acc, prev_info)) = &prev_valid {
                    let mut label1 = prev_info.label.clone();
                    let mut label2 = curr_info.label.clone();
                    let mut addr1 = prev_acc.addr;
                    let mut addr2 = curr_acc.addr;

                    if label1 > label2 {
                        std::mem::swap(&mut label1, &mut label2);
                        std::mem::swap(&mut addr1, &mut addr2);
                    }

                    let stat = edge_map.entry((label1, label2)).or_default();
                    stat.addr1 = addr1;
                    stat.addr2 = addr2;

                    let same_line = (prev_acc.addr / config.cache_line_size)
                        == (curr_acc.addr / config.cache_line_size);

                    if prev_acc.core_id != curr_acc.core_id {
                        if prev_acc.rw == Rw::Write {
                            if same_line {
                                stat.count1 += 1;
                            } else {
                                stat.count2 += 1;
                            }
                        } else {
                            stat.count3 += 1;
                        }
                    } else {
                        stat.count4 += 1;
                    }
                }
                prev_valid = Some((curr_acc, curr_info));
            } else {
                if warn_limit < 20 {
                    eprintln!("Warning: could not resolve label for addr 0x{:x}", curr_acc.addr);
                    warn_limit += 1;
                    if warn_limit == 20 {
                        eprintln!("Warning: could not resolve label for addr... (silencing further warnings)");
                    }
                }
            }
        }
    }

    let get_group = |label: &str| -> Option<String> {
        if let Some(idx) = label.find('[') {
            Some(label[..idx].to_string())
        } else {
            None
        }
    };

    let mut groups: std::collections::HashSet<Option<String>> = std::collections::HashSet::new();
    for info in node_map.values() {
        groups.insert(get_group(&info.label));
    }

    let out_path = std::path::Path::new(&config.output);
    let parent = out_path.parent().unwrap_or_else(|| std::path::Path::new(""));
    let stem = out_path.file_stem().unwrap_or_default().to_str().unwrap_or_default();
    let ext = out_path.extension().unwrap_or_default().to_str().unwrap_or_default();
    let ext_with_dot = if ext.is_empty() { String::new() } else { format!(".{}", ext) };

    for group_opt in groups {
        let output_file_path = match &group_opt {
            Some(arr_name) => {
                let mut p = parent.to_path_buf();
                p.push(format!("{}.{}{}", stem, arr_name, ext_with_dot));
                p
            }
            None => out_path.to_path_buf(),
        };

        let mut group_edges = Vec::new();
        let mut group_nodes_set = std::collections::HashSet::new();

        for ((l1, l2), stat) in &edge_map {
            let g1 = get_group(l1);
            let g2 = get_group(l2);

            let include_edge = if group_opt.is_none() {
                g1.is_none() && g2.is_none()
            } else {
                g1 == group_opt || g2 == group_opt
            };

            if include_edge {
                group_edges.push(((l1.clone(), l2.clone()), stat.clone()));
                group_nodes_set.insert(l1.clone());
                group_nodes_set.insert(l2.clone());
            }
        }

        for info in node_map.values() {
            if get_group(&info.label) == group_opt {
                group_nodes_set.insert(info.label.clone());
            }
        }

        if group_nodes_set.is_empty() {
            continue;
        }

        let mut out = File::create(&output_file_path).unwrap_or_else(|e| {
            eprintln!("Error creating output file: {}", e);
            exit(1);
        });

        writeln!(out, "graph G {{").unwrap();
        writeln!(out, "  node [shape=box];\n").unwrap();

        let mut nodes: Vec<_> = group_nodes_set
            .into_iter()
            .filter_map(|lbl| node_map.get(&lbl))
            .collect();
        nodes.sort_by(|a, b| a.label.cmp(&b.label));

        for info in nodes {
            writeln!(
                out,
                "  \"{}\" [label=\"{}\", type=\"{}\", offset={}];",
                escape_dot_string(&info.label),
                escape_dot_string(&info.label),
                escape_dot_string(&info.typ),
                info.offset
            )
            .unwrap();
        }
        writeln!(out, "").unwrap();

        group_edges.sort_by(|(k1, _), (k2, _)| k1.cmp(k2));

        for ((label1, label2), stat) in group_edges {
            if config.disable_self_weight && label1 == label2 {
                continue;
            }

            let alpha = stat.count1 + stat.count2;
            let beta = stat.count3 + stat.count4;
            let same_line = (stat.addr1 / config.cache_line_size) == (stat.addr2 / config.cache_line_size);
            let alpha_kind = if same_line { "①" } else { "②" };

            writeln!(
                out,
                "  \"{}\" -- \"{}\" [",
                escape_dot_string(&label1),
                escape_dot_string(&label2)
            )
            .unwrap();
            writeln!(out, "    label=\"({},{})\",", alpha, beta).unwrap();
            writeln!(out, "    alpha={},", alpha).unwrap();
            writeln!(out, "    beta={},", beta).unwrap();
            writeln!(out, "    count1={},", stat.count1).unwrap();
            writeln!(out, "    count2={},", stat.count2).unwrap();
            writeln!(out, "    count3={},", stat.count3).unwrap();
            writeln!(out, "    count4={},", stat.count4).unwrap();
            writeln!(out, "    alpha_kind=\"{}\"", alpha_kind).unwrap();
            writeln!(out, "  ];").unwrap();
        }

        writeln!(out, "}}").unwrap();
    }
}
