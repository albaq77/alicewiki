use std::env;
use std::fs::File;
use std::io::{BufReader, Read, Write};
use std::collections::HashMap;
use std::path::Path;
use regex::Regex;

fn guess_base_size(mut t: &str) -> u64 {
    t = t.trim();
    if t.starts_with("volatile ") { t = &t[9..]; }
    if t.starts_with("const ") { t = &t[6..]; }
    t = t.trim();
    if t.contains('*') { return 8; }
    match t {
        "char" | "unsigned char" | "int8_t" | "uint8_t" => 1,
        "short" | "unsigned short" | "int16_t" | "uint16_t" => 2,
        "int" | "unsigned int" | "int32_t" | "uint32_t" | "float" => 4,
        "long" | "unsigned long" | "long long" | "unsigned long long" | "int64_t" | "uint64_t" | "double" => 8,
        _ => 8,
    }
}

#[derive(Debug, Clone)]
struct Node {
    id: usize,
    name: String,
    typ: String,
    size: u64,
}

fn extract_field_name(name: &str) -> String {
    // If it's Point::x, return x
    if let Some(idx) = name.rfind("::") {
        return name[idx + 2..].to_string();
    }
    // If it's team[0].health, return health
    if let Some(idx) = name.rfind('.') {
        return name[idx + 1..].to_string();
    }
    // If it's just global_counter, return global_counter
    name.to_string()
}

fn get_base_name(dot_file: &str) -> String {
    let path = Path::new(dot_file);
    let file_name = path.file_name().unwrap_or_default().to_string_lossy();
    file_name.replace("graph.", "").replace(".fields.dot", "").replace(".dot", "")
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 6 {
        eprintln!("Usage: package_fields <dot_file> <alpha> <beta> <gamma> <cacheline>");
        std::process::exit(1);
    }

    let dot_file = &args[1];
    let alpha_cli: f64 = args[2].parse().unwrap_or(1.0);
    let beta_cli: f64 = args[3].parse().unwrap_or(1.0);
    let gamma_cli: f64 = args[4].parse().unwrap_or(1.0);
    let cacheline: u64 = args[5].parse().unwrap_or(64);

    let base_target = get_base_name(dot_file);

    let file = match File::open(dot_file) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Error opening file '{}': {}", dot_file, e);
            std::process::exit(1);
        }
    };
    let mut reader = BufReader::new(file);
    let mut file_content = String::new();
    let _ = reader.read_to_string(&mut file_content);

    let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[.*?type="([^"]+)".*?\];"#).unwrap();
    let edge_re = Regex::new(r#""([^"]+)"\s*--\s*"([^"]+)"\s*\[[^\]]*?label="\(([\d\.]+),([\d\.]+)\)""#).unwrap();

    let mut abstract_nodes: HashMap<String, Node> = HashMap::new();
    let mut raw_to_abstract: HashMap<String, String> = HashMap::new();
    let mut field_names_vec: Vec<String> = Vec::new();

    for line in file_content.lines() {
        if let Some(caps) = node_re.captures(line) {
            let raw_name = caps[1].to_string();
            let typ = caps[2].to_string();
            
            // Filtering out external nodes correctly
            if dot_file.ends_with(".fields.dot") {
                let struct_prefix = format!("{}::", base_target);
                if !raw_name.starts_with(&struct_prefix) {
                    continue;
                }
            } else {
                // E.g. graph.team.dot
                let array_prefix = format!("{}[", base_target);
                if !raw_name.starts_with(&array_prefix) {
                    continue; // Skip single globals and cross-struct nodes
                }
            }

            let field_name = extract_field_name(&raw_name);
            raw_to_abstract.insert(raw_name.clone(), field_name.clone());

            if !abstract_nodes.contains_key(&field_name) {
                let size = guess_base_size(&typ);
                let id = field_names_vec.len();
                field_names_vec.push(field_name.clone());
                abstract_nodes.insert(field_name.clone(), Node { id, name: field_name, typ, size });
            }
        }
    }

    if field_names_vec.is_empty() {
        eprintln!("Error: No struct fields extracted from {}.", dot_file);
        std::process::exit(1);
    }

    let n = field_names_vec.len();

    let edge_head_re = Regex::new(r#"(?m)^\s*"[^"]+"\s*--\s*"[^"]+"\s*\["#).unwrap();
    let k_edges = edge_head_re.find_iter(&file_content).count();
    let m_edges = edge_re.captures_iter(&file_content).count();
    if m_edges < k_edges {
        eprintln!(
            "Error: 边读取失败 -- '{}' 中有 {} 条边, 只解析出 {} 条。DOT 属性格式可能已变更 (label 写法或属性顺序)。",
            dot_file, k_edges, m_edges
        );
        std::process::exit(1);
    }
    if k_edges == 0 && n > 1 {
        eprintln!("Warning: '{}' 中没有边 (节点 {} 个), 图本身可能是空的。", dot_file, n);
    }
    println!("[edges] {}: parsed {}/{}", dot_file, m_edges, k_edges);

    let mut w_matrix = vec![vec![0.0; n]; n];

    for caps in edge_re.captures_iter(&file_content) {
        let u_raw = caps[1].to_string();
        let v_raw = caps[2].to_string();
        
        if let (Some(u_field), Some(v_field)) = (raw_to_abstract.get(&u_raw), raw_to_abstract.get(&v_raw)) {
            let u_id = abstract_nodes[u_field].id;
            let v_id = abstract_nodes[v_field].id;
            
            let a: f64 = caps[3].parse().unwrap_or(0.0);
            let b: f64 = caps[4].parse().unwrap_or(0.0);
            
            let w_same = alpha_cli * b - beta_cli * a;
            let w_diff = alpha_cli * (a + b) / gamma_cli;
            let delta_w = w_same - w_diff;
            
            w_matrix[u_id][v_id] += delta_w;
            if u_id != v_id {
                w_matrix[v_id][u_id] += delta_w; // Undirected
            }
        }
    }

    let mut bin_of: Vec<usize> = (0..n).collect();
    let mut bin_size: Vec<u64> = field_names_vec.iter().map(|f| abstract_nodes[f].size).collect();
    let mut active_bins: Vec<bool> = vec![true; n];

    loop {
        let mut max_w = 0.0;
        let mut best_pair = None;

        for i in 0..n {
            if !active_bins[i] { continue; }
            for j in (i+1)..n {
                if !active_bins[j] { continue; }
                
                if bin_size[i] + bin_size[j] <= cacheline {
                    if w_matrix[i][j] > max_w {
                        max_w = w_matrix[i][j];
                        best_pair = Some((i, j));
                    }
                }
            }
        }

        match best_pair {
            Some((u, v)) => {
                active_bins[v] = false;
                bin_size[u] += bin_size[v];
                for i in 0..n { if bin_of[i] == v { bin_of[i] = u; } }
                for i in 0..n {
                    if active_bins[i] && i != u && i != v {
                        let w_sum = w_matrix[u][i] + w_matrix[v][i];
                        w_matrix[u][i] = w_sum;
                        w_matrix[i][u] = w_sum;
                    }
                }
            }
            None => break,
        }
    }

    loop {
        let mut best_pair = None;
        for i in 0..n {
            if !active_bins[i] { continue; }
            for j in (i+1)..n {
                if !active_bins[j] { continue; }
                if bin_size[i] + bin_size[j] <= cacheline {
                    if w_matrix[i][j] == 0.0 {
                        best_pair = Some((i, j));
                        break;
                    }
                }
            }
            if best_pair.is_some() { break; }
        }

        match best_pair {
            Some((u, v)) => {
                active_bins[v] = false;
                bin_size[u] += bin_size[v];
                for i in 0..n { if bin_of[i] == v { bin_of[i] = u; } }
                for i in 0..n {
                    if active_bins[i] && i != u && i != v {
                        let w_sum = w_matrix[u][i] + w_matrix[v][i];
                        w_matrix[u][i] = w_sum;
                        w_matrix[i][u] = w_sum;
                    }
                }
            }
            None => break,
        }
    }

    let mut bins: HashMap<usize, Vec<String>> = HashMap::new();
    for i in 0..n {
        bins.entry(bin_of[i]).or_default().push(field_names_vec[i].clone());
    }

    let mut final_groups: Vec<Vec<String>> = bins.into_values().collect();
    final_groups.sort_by_key(|group| group[0].clone());

    let out_path = dot_file.replace(".fields.dot", ".layout.txt").replace(".dot", ".layout.txt");
    let mut out_file = File::create(&out_path).unwrap();

    writeln!(out_file, "Target: {}", base_target).unwrap();
    writeln!(out_file, "Cacheline size: {}", cacheline).unwrap();
    writeln!(out_file, "========================================").unwrap();

    let mut cacheline_id = 0;
    for group in final_groups {
        writeln!(out_file, "[Cacheline {}]", cacheline_id).unwrap();
        for field in group {
            writeln!(out_file, "  - {}", field).unwrap();
        }
        cacheline_id += 1;
    }
    println!("Successfully generated {}", out_path);
}
