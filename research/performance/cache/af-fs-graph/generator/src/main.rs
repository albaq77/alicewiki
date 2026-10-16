use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::process::{exit, Command};

#[derive(Debug)]
struct Config {
    input: String,
    output: String,
    binary: String,
    cache_line_size: u64,
    dbg_viewtype: String,
    disable_self_weight: bool,
    window: u64,
}

fn print_usage_and_exit(prog_name: &str) -> ! {
    eprintln!(
        "Usage: {} -i <trace.csv> -o <graph.dot> --binary <app> [--cache-line-size <size>] [--dbg-viewtype <path>] [--disable-self-weight] [--window <ns>]",
        prog_name
    );
    exit(1);
}

fn parse_args() -> Config {
    let args: Vec<String> = env::args().collect();
    let prog_name = args
        .get(0)
        .cloned()
        .unwrap_or_else(|| "generator".to_string());

    let mut input = None;
    let mut output = None;
    let mut binary = None;
    let mut cache_line_size = 64;
    let mut dbg_viewtype = "dbg-viewtype".to_string();
    let mut disable_self_weight = false;
    let mut window = 0u64;

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
        } else if arg == "--window" {
            if i + 1 < args.len() {
                window = args[i + 1].parse().unwrap_or(u64::MAX);
                if window == u64::MAX {
                    eprintln!("Error: --window expects a non-negative integer of nanoseconds.");
                    exit(1);
                }
                i += 1;
            } else {
                print_usage_and_exit(&prog_name);
            }
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
        window,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Rw {
    Read,
    Write,
}

#[derive(Debug, Clone)]
struct Access {
    time: u64,
    addr: u64,
    rw: Rw,
    size: u64,
    core_id: u32,
    thread_id: u64,
}

impl Access {
    fn parse(line: &str) -> Option<Self> {
        let parts: Vec<&str> = line.split(',').map(|s| s.trim()).collect();
        if parts.len() < 6 {
            eprintln!("Warning: invalid line format, skipping: {}", line);
            return None;
        }

        let time = parts[0].parse::<u64>().unwrap_or(0);
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

        let thread_id = match parts[5].parse::<u64>() {
            Ok(t) => t,
            Err(_) => {
                eprintln!("Warning: invalid thread_id, skipping: {}", line);
                return None;
            }
        };

        Some(Access {
            time,
            addr,
            rw,
            size,
            core_id,
            thread_id,
        })
    }
}

fn classify_pair(prev: &Access, curr: &Access, cache_line_size: u64) -> (u8, bool) {
    let prev_end = prev.addr.saturating_add(prev.size);
    let curr_end = curr.addr.saturating_add(curr.size);
    let same_line = prev.size > 0
        && curr.size > 0
        && prev.addr / cache_line_size <= curr_end.saturating_sub(1) / cache_line_size
        && curr.addr / cache_line_size <= prev_end.saturating_sub(1) / cache_line_size;
    let overlaps = prev.addr < curr_end && curr.addr < prev_end;
    let cross_worker = prev.core_id != curr.core_id && prev.thread_id != curr.thread_id;
    let involves_write = prev.rw == Rw::Write || curr.rw == Rw::Write;
    let bucket = if cross_worker && involves_write {
        if same_line {
            1
        } else {
            2
        }
    } else if cross_worker {
        3
    } else {
        4
    };
    (bucket, overlaps)
}

fn sharing_provenance(bucket: u8, overlaps: bool) -> (u64, u64) {
    if bucket != 1 {
        (0, 0)
    } else if overlaps {
        (1, 0)
    } else {
        (0, 1)
    }
}

#[derive(Clone, Debug, Hash, PartialEq, Eq)]
struct NodeInfo {
    label: String,
    typ: String,
    offset: i64,
    struct_name: Option<String>,
    dims: Vec<u64>,
    elem_size: u64,
    byte_size: u64,
    base_address: u64,
    member: Option<StructMember>,
}

#[derive(Debug, Default)]
struct EdgeStat {
    count1: u64,
    count2: u64,
    count3: u64,
    count4: u64,
    true_alpha: u64,
    false_alpha: u64,
    addr1: u64,
    addr2: u64,
}

#[derive(Debug, Clone, Hash, PartialEq, Eq)]
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
                        st.members
                            .insert(offset, StructMember { offset, name, typ });
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
                map.insert(addr, GlobalVar { name, typ });
            }
        }
    }
    map
}

#[path = "../../type_layout.rs"]
mod type_layout;

fn guess_base_size(type_str: &str) -> Option<u64> {
    type_layout::type_layout(type_str).map(|layout| layout.0)
}

// Array bounds and element sizes come from types, never the gap to the next
// member: that gap may contain alignment padding.
fn type_shape(name: &str) -> Option<(String, Vec<u64>)> {
    let mut base = name.trim();
    let mut dims = Vec::new();
    while base.ends_with(']') {
        let start = base.rfind('[')?;
        let bound = base[start + 1..base.len() - 1].trim().parse().ok()?;
        if bound == 0 {
            return None;
        }
        dims.push(bound);
        base = base[..start].trim();
    }
    dims.reverse();
    let base = base
        .split_whitespace()
        .filter(|s| !matches!(*s, "const" | "volatile" | "restrict" | "_Atomic"))
        .collect::<Vec<_>>()
        .join(" ");
    Some((base, dims))
}

fn known_size(name: &str, types: &HashMap<String, StructType>) -> Option<u64> {
    let (base, dims) = type_shape(name)?;
    let size = guess_base_size(&base).or_else(|| types.get(&base).map(|s| s.size))?;
    dims.iter()
        .try_fold(size, |size, bound| size.checked_mul(*bound))
        .filter(|s| *s > 0)
}

fn append_indices(label: &mut String, mut index: u64, dims: &[u64]) {
    let mut indices = Vec::new();
    for bound in dims.iter().rev() {
        indices.push(index % bound);
        index /= bound;
    }
    for i in indices.iter().rev() {
        label.push_str(&format!("[{}]", i));
    }
}

// A node represents one typed leaf access. Bulk operations spanning leaves,
// accesses into implicit padding, and unknown objects are explicitly skipped;
// they must not be folded into a neighbouring member or a wrapped array index.
fn resolve_leaf(
    typ: &str,
    delta: u64,
    size: u64,
    types: &HashMap<String, StructType>,
    label: &mut String,
    member: &mut Option<StructMember>,
    depth: usize,
) -> Option<String> {
    if depth > 32 || delta.checked_add(size)? > known_size(typ, types)? {
        return None;
    }
    let (base, dims) = type_shape(typ)?;
    if !dims.is_empty() {
        let element_size = known_size(&base, types)?;
        let within = delta % element_size;
        if within.checked_add(size)? > element_size {
            return None;
        }
        append_indices(label, delta / element_size, &dims);
        return resolve_leaf(&base, within, size, types, label, member, depth + 1);
    }
    if let Some(st) = types.get(&base) {
        let (_, field) = st.members.range(..=delta).next_back()?;
        let within = delta.checked_sub(field.offset)?;
        if within.checked_add(size)? > known_size(&field.typ, types)? {
            return None;
        }
        if member.is_none() {
            *member = Some(field.clone());
        }
        label.push_str(&format!(".{}", field.name));
        return resolve_leaf(&field.typ, within, size, types, label, member, depth + 1);
    }
    if delta != 0 || size != known_size(&base, types)? {
        label.push_str(&format!("+{}:{}", delta, size));
    }
    Some(base)
}

fn resolve_access(
    addr: u64,
    size: u64,
    vars: &BTreeMap<u64, GlobalVar>,
    types: &HashMap<String, StructType>,
) -> Option<NodeInfo> {
    if size == 0 {
        return None;
    }
    addr.checked_add(size)?;
    let (&var_addr, var) = vars.range(..=addr).next_back()?;
    let delta = addr.checked_sub(var_addr)?;
    if delta.checked_add(size)? > known_size(&var.typ, types)? {
        return None;
    }
    let (base, dims) = type_shape(&var.typ)?;
    let elem_size = known_size(&base, types)?;
    let mut label = var.name.clone();
    let within = if dims.is_empty() {
        delta
    } else {
        let within = delta % elem_size;
        if within.checked_add(size)? > elem_size {
            return None;
        }
        append_indices(&mut label, delta / elem_size, &dims);
        within
    };
    let mut member = None;
    let typ = resolve_leaf(&base, within, size, types, &mut label, &mut member, 0)?;
    Some(NodeInfo {
        label,
        typ,
        offset: within as i64,
        struct_name: types.get(&base).map(|st| st.name.clone()),
        elem_size: if dims.is_empty() { 0 } else { elem_size },
        dims,
        byte_size: size,
        base_address: var_addr,
        member,
    })
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
            eprintln!("Error: dbg-viewtype exited with status {}", s);
            exit(1);
        }
        Err(e) => {
            eprintln!("Error: failed to execute dbg-viewtype: {}", e);
            exit(1);
        }
    }

    let types_map = parse_types(type_out);
    let vars_map = parse_vars(var_out);
    if vars_map.is_empty() {
        eprintln!("Error: DWARF contains no resolvable global variables");
        exit(1);
    }

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

    let mut window_hist: VecDeque<(Access, NodeInfo)> = VecDeque::new();
    let window_max: usize = if config.window == 0 { 1 } else { 4096 };
    let mut warn_limit = 0;
    let mut parsed_accesses = 0u64;
    let mut resolved_accesses = 0u64;

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
            parsed_accesses += 1;
            if let Some(curr_info) =
                resolve_access(curr_acc.addr, curr_acc.size, &vars_map, &types_map)
            {
                resolved_accesses += 1;
                node_map
                    .entry(curr_info.label.clone())
                    .or_insert_with(|| curr_info.clone());

                if config.window > 0 {
                    loop {
                        let front_time = match window_hist.front() {
                            Some(f) => f.0.time,
                            None => break,
                        };
                        if curr_acc.time.saturating_sub(front_time) > config.window {
                            window_hist.pop_front();
                        } else {
                            break;
                        }
                    }
                }

                let mut seen: HashSet<String> = HashSet::new();
                for (prev_acc, prev_info) in window_hist.iter().rev() {
                    if !seen.insert(prev_info.label.clone()) {
                        continue;
                    }
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

                    let (bucket, overlaps) =
                        classify_pair(prev_acc, &curr_acc, config.cache_line_size);
                    match bucket {
                        1 => stat.count1 += 1,
                        2 => stat.count2 += 1,
                        3 => stat.count3 += 1,
                        _ => stat.count4 += 1,
                    }
                    // False sharing requires the same physical cache line.  A
                    // different-address pair in bucket 2 is cross-line traffic,
                    // not false sharing.
                    let (true_alpha, false_alpha) = sharing_provenance(bucket, overlaps);
                    stat.true_alpha += true_alpha;
                    stat.false_alpha += false_alpha;
                }
                window_hist.push_back((curr_acc, curr_info));
                while window_hist.len() > window_max {
                    window_hist.pop_front();
                }
            } else {
                if warn_limit < 20 {
                    eprintln!(
                        "Warning: could not resolve label for addr 0x{:x}",
                        curr_acc.addr
                    );
                    warn_limit += 1;
                    if warn_limit == 20 {
                        eprintln!("Warning: could not resolve label for addr... (silencing further warnings)");
                    }
                }
            }
        }
    }

    println!(
        "[accesses] parsed={} resolved={} skipped={}",
        parsed_accesses,
        resolved_accesses,
        parsed_accesses - resolved_accesses
    );
    if resolved_accesses == 0 {
        eprintln!("Error: no accesses resolved within known object/member bounds");
        exit(1);
    }

    let mut groups: std::collections::HashSet<Option<String>> = std::collections::HashSet::new();
    for info in node_map.values() {
        let mut added = false;
        if let Some(sname) = &info.struct_name {
            groups.insert(Some(format!("{}.fields", sname)));
            added = true;
        }
        if let Some(idx) = info.label.find('[').filter(|_| !info.dims.is_empty()) {
            groups.insert(Some(info.label[..idx].to_string()));
            added = true;
        }
        if !added {
            groups.insert(None);
        }
    }

    let out_path = std::path::Path::new(&config.output);
    let parent = out_path
        .parent()
        .unwrap_or_else(|| std::path::Path::new(""));
    let stem = out_path
        .file_stem()
        .unwrap_or_default()
        .to_str()
        .unwrap_or_default();
    let ext = out_path
        .extension()
        .unwrap_or_default()
        .to_str()
        .unwrap_or_default();
    let ext_with_dot = if ext.is_empty() {
        String::new()
    } else {
        format!(".{}", ext)
    };

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
            let info1 = node_map.get(l1).unwrap();
            let info2 = node_map.get(l2).unwrap();
            let belongs_to = |info: &NodeInfo, l: &str, g_opt: &Option<String>| -> bool {
                match g_opt {
                    None => info.struct_name.is_none() && !l.contains('['),
                    Some(ref g) => {
                        if g.ends_with(".fields") {
                            if let Some(sname) = &info.struct_name {
                                if g == &format!("{}.fields", sname) {
                                    return true;
                                }
                            }
                        } else {
                            if let Some(idx) = l.find('[').filter(|_| !info.dims.is_empty()) {
                                if g == &l[..idx] {
                                    return true;
                                }
                            }
                        }
                        false
                    }
                }
            };
            // Analysis graphs are closed subgraphs.  Pulling an external endpoint
            // into graph.dot made struct fields look like standalone globals and
            // also injected cross-object scheduling noise into array decisions.
            let include_edge =
                belongs_to(info1, l1, &group_opt) && belongs_to(info2, l2, &group_opt);

            if include_edge {
                group_edges.push(((l1.clone(), l2.clone()), stat));
                group_nodes_set.insert(l1.clone());
                group_nodes_set.insert(l2.clone());
            }
        }

        let belongs_to2 = |info: &NodeInfo, l: &str, g_opt: &Option<String>| -> bool {
            match g_opt {
                None => info.struct_name.is_none() && !l.contains('['),
                Some(ref g) => {
                    if g.ends_with(".fields") {
                        if let Some(sname) = &info.struct_name {
                            if g == &format!("{}.fields", sname) {
                                return true;
                            }
                        }
                    } else {
                        if let Some(idx) = l.find('[').filter(|_| !info.dims.is_empty()) {
                            if g == &l[..idx] {
                                return true;
                            }
                        }
                    }
                    false
                }
            }
        };
        for (lbl, info) in &node_map {
            if belongs_to2(info, lbl, &group_opt) {
                group_nodes_set.insert(lbl.clone());
            }
        }

        if group_nodes_set.is_empty() {
            continue;
        }

        let mut out = File::create(&output_file_path).unwrap_or_else(|e| {
            eprintln!("Error creating output file: {}", e);
            std::process::exit(1);
        });

        writeln!(out, "graph G {{").unwrap();
        writeln!(out, "  node [shape=box];\n").unwrap();

        let abstract_label = |lbl: String, info: &NodeInfo| -> String {
            if let Some(ref g) = group_opt {
                if g.ends_with(".fields") {
                    if let Some(ref info_sname) = info.struct_name {
                        if let Some(member) = &info.member {
                            return format!("{}::{}", info_sname, member.name);
                        }
                    }
                }
            }
            lbl
        };

        let mut abstract_nodes: std::collections::HashMap<String, NodeInfo> =
            std::collections::HashMap::new();
        for lbl in group_nodes_set {
            if let Some(info) = node_map.get(&lbl) {
                let albl = abstract_label(lbl.clone(), info);
                if !abstract_nodes.contains_key(&albl) {
                    let mut ainfo = info.clone();
                    ainfo.label = albl.clone();
                    if group_opt.as_ref().is_some_and(|g| g.ends_with(".fields")) {
                        if let Some(member) = &info.member {
                            ainfo.typ = member.typ.clone();
                            ainfo.offset = member.offset as i64;
                            ainfo.byte_size = known_size(&member.typ, &types_map).unwrap_or(0);
                            ainfo.base_address = 0;
                            ainfo.dims.clear();
                            ainfo.elem_size = 0;
                        }
                    }
                    abstract_nodes.insert(albl, ainfo);
                }
            }
        }

        // Dynamic tracing cannot discover cold fields.  Complete field graphs
        // from DWARF so a recommended struct layout never drops unobserved data.
        if let Some(group_name) = &group_opt {
            if let Some(struct_name) = group_name.strip_suffix(".fields") {
                if let Some(struct_type) = types_map.get(struct_name) {
                    for member in struct_type.members.values() {
                        let label = format!("{}::{}", struct_name, member.name);
                        abstract_nodes.entry(label.clone()).or_insert(NodeInfo {
                            label,
                            typ: member.typ.clone(),
                            offset: member.offset as i64,
                            struct_name: Some(struct_name.to_string()),
                            dims: Vec::new(),
                            elem_size: 0,
                            byte_size: known_size(&member.typ, &types_map).unwrap_or(0),
                            base_address: 0,
                            member: Some(member.clone()),
                        });
                    }
                }
            }
        }

        let mut nodes: Vec<_> = abstract_nodes.into_values().collect();
        nodes.sort_by(|a, b| a.label.cmp(&b.label));

        for info in nodes {
            let mut attributes = format!(
                "label=\"{}\", type=\"{}\", offset={}, size={}, base_address={}",
                escape_dot_string(&info.label),
                escape_dot_string(&info.typ),
                info.offset,
                info.byte_size,
                info.base_address
            );
            if let Some(member) = &info.member {
                attributes.push_str(&format!(
                    ", field=\".{}\", field_offset={}, field_size={}",
                    escape_dot_string(&member.name),
                    member.offset,
                    known_size(&member.typ, &types_map).unwrap_or(0)
                ));
            }
            if !info.dims.is_empty() {
                let declared: Vec<String> = info.dims.iter().map(|d| d.to_string()).collect();
                attributes.push_str(&format!(", dims=\"{}\"", declared.join(",")));
            }
            if info.elem_size > 0 {
                attributes.push_str(&format!(", elem_size={}", info.elem_size));
            }
            writeln!(
                out,
                "  \"{}\" [{}];",
                escape_dot_string(&info.label),
                attributes
            )
            .unwrap();
        }
        writeln!(out, "").unwrap();

        let mut aggregated_edges: std::collections::HashMap<(String, String), EdgeStat> =
            std::collections::HashMap::new();
        for ((l1, l2), stat) in group_edges {
            let info1 = node_map.get(&l1).unwrap();
            let info2 = node_map.get(&l2).unwrap();
            let al1 = abstract_label(l1.clone(), info1);
            let al2 = abstract_label(l2.clone(), info2);

            let (k1, k2) = if al1 <= al2 { (al1, al2) } else { (al2, al1) };

            let e = aggregated_edges.entry((k1, k2)).or_insert(EdgeStat {
                addr1: stat.addr1,
                addr2: stat.addr2,
                count1: 0,
                count2: 0,
                count3: 0,
                count4: 0,
                true_alpha: 0,
                false_alpha: 0,
            });

            // count1/count2 describe cache-line geometry, not endpoint order.
            // Canonicalising an undirected key must never exchange them.
            e.count1 += stat.count1;
            e.count2 += stat.count2;
            e.count3 += stat.count3;
            e.count4 += stat.count4;
            e.true_alpha += stat.true_alpha;
            e.false_alpha += stat.false_alpha;
        }

        let mut final_edges: Vec<_> = aggregated_edges.into_iter().collect();
        final_edges.sort_by(|(k1, _), (k2, _)| k1.cmp(k2));

        for ((label1, label2), stat) in final_edges {
            if config.disable_self_weight && label1 == label2 {
                continue;
            }

            let alpha = stat.count1 + stat.count2;
            let beta = stat.count3 + stat.count4;
            let alpha_kind = match (stat.count1 > 0, stat.count2 > 0) {
                (true, true) => "mixed",
                (true, false) => "①",
                (false, true) => "②",
                (false, false) => "none",
            };

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
            writeln!(out, "    true_alpha={},", stat.true_alpha).unwrap();
            writeln!(out, "    false_alpha={},", stat.false_alpha).unwrap();
            writeln!(out, "    alpha_kind=\"{}\"", alpha_kind).unwrap();
            writeln!(out, "  ];").unwrap();
        }

        writeln!(out, "}}").unwrap();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn access(addr: u64, rw: Rw, core_id: u32, thread_id: u64) -> Access {
        Access {
            time: 0,
            addr,
            rw,
            size: 8,
            core_id,
            thread_id,
        }
    }

    #[test]
    fn category_one_covers_cross_worker_write_on_same_line() {
        let prev = access(0x100, Rw::Write, 0, 10);
        let curr = access(0x108, Rw::Read, 1, 11);
        assert_eq!(classify_pair(&prev, &curr, 64), (1, false));
    }

    #[test]
    fn read_then_write_is_invalidation_not_read_only_affinity() {
        let prev = access(0x100, Rw::Read, 0, 10);
        let curr = access(0x108, Rw::Write, 1, 11);
        assert_eq!(classify_pair(&prev, &curr, 64), (1, false));
    }

    #[test]
    fn overlapping_address_is_true_sharing() {
        let prev = access(0x100, Rw::Write, 0, 10);
        let curr = access(0x104, Rw::Read, 1, 11);
        assert_eq!(classify_pair(&prev, &curr, 64), (1, true));
    }

    #[test]
    fn migration_of_one_thread_is_not_cross_worker_contention() {
        let prev = access(0x100, Rw::Write, 0, 10);
        let curr = access(0x108, Rw::Read, 1, 10);
        assert_eq!(classify_pair(&prev, &curr, 64), (4, false));
    }

    #[test]
    fn cross_line_pair_is_not_false_sharing() {
        assert_eq!(sharing_provenance(2, false), (0, 0));
        assert_eq!(sharing_provenance(1, false), (0, 1));
        assert_eq!(sharing_provenance(1, true), (1, 0));
    }

    #[test]
    fn spanning_access_shares_its_second_cacheline() {
        let prev = access(0x13c, Rw::Read, 0, 10);
        let curr = access(0x140, Rw::Write, 1, 11);
        assert_eq!(classify_pair(&prev, &curr, 64), (1, true));
    }

    #[test]
    fn array_struct_offset_is_relative_to_one_element() {
        let mut vars = BTreeMap::new();
        vars.insert(
            0x1000,
            GlobalVar {
                name: "entities".to_string(),
                typ: "Entity[2][2]".to_string(),
            },
        );
        let mut members = BTreeMap::new();
        members.insert(
            0,
            StructMember {
                offset: 0,
                name: "health".to_string(),
                typ: "long".to_string(),
            },
        );
        members.insert(
            64,
            StructMember {
                offset: 64,
                name: "mana".to_string(),
                typ: "long".to_string(),
            },
        );
        let mut types = HashMap::new();
        types.insert(
            "Entity".to_string(),
            StructType {
                name: "Entity".to_string(),
                size: 320,
                members,
            },
        );
        let info = resolve_access(0x1000 + 320 + 64, 8, &vars, &types).unwrap();
        assert_eq!(info.label, "entities[0][1].mana");
        assert_eq!(info.offset, 64);
        assert_eq!(info.dims, vec![2, 2]);
        assert_eq!(info.elem_size, 320);
    }
}
