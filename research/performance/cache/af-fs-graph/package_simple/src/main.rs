use std::env;
use std::fs::File;
use std::io::{BufReader, Read};
use std::collections::HashMap;
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

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 6 {
        eprintln!("Usage: package_simple <dot_file> <alpha> <beta> <gamma> <cacheline>");
        std::process::exit(1);
    }

    let dot_file = &args[1];
    let alpha_cli: f64 = args[2].parse().unwrap_or(1.0);
    let beta_cli: f64 = args[3].parse().unwrap_or(1.0);
    let gamma_cli: f64 = args[4].parse().unwrap_or(1.0);
    let cacheline: u64 = args[5].parse().unwrap_or(64);

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

    let mut nodes_map: HashMap<String, Node> = HashMap::new();
    let mut nodes: Vec<Node> = Vec::new();

    for line in file_content.lines() {
        if let Some(caps) = node_re.captures(line) {
            let name = caps[1].to_string();
            let typ = caps[2].to_string();
            
            if name.contains('[') {
                continue; // Skip arrays if they slipped in
            }
            
            let size = guess_base_size(&typ);
            let id = nodes.len();
            let node = Node { id, name: name.clone(), typ, size };
            nodes_map.insert(name, node.clone());
            nodes.push(node);
        }
    }

    if nodes.is_empty() {
        eprintln!("Error: No single-variable nodes found in the dot file.");
        std::process::exit(1);
    }

    let n = nodes.len();
    let mut w_matrix = vec![vec![0.0; n]; n];

    for caps in edge_re.captures_iter(&file_content) {
        let u_name = caps[1].to_string();
        let v_name = caps[2].to_string();
        
        if let (Some(u), Some(v)) = (nodes_map.get(&u_name), nodes_map.get(&v_name)) {
            let a: f64 = caps[3].parse().unwrap_or(0.0);
            let b: f64 = caps[4].parse().unwrap_or(0.0);
            
            let w_same = alpha_cli * b - beta_cli * a;
            let w_diff = alpha_cli * (a + b) / gamma_cli;
            let delta_w = w_same - w_diff;
            
            w_matrix[u.id][v.id] += delta_w;
            w_matrix[v.id][u.id] += delta_w; // Undirected
        }
    }

    // Graph partitioning / agglomerative clustering
    let mut bin_of: Vec<usize> = (0..n).collect();
    let mut bin_size: Vec<u64> = nodes.iter().map(|nd| nd.size).collect();
    let mut active_bins: Vec<bool> = vec![true; n];

    // Pass 1: Merge pairs with maximum positive benefit
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
                
                for i in 0..n {
                    if bin_of[i] == v {
                        bin_of[i] = u;
                    }
                }
                
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

    // Pass 2: Merge non-interacting pairs (weight == 0.0) to pack compactly
    loop {
        let mut best_pair = None;

        for i in 0..n {
            if !active_bins[i] { continue; }
            for j in (i+1)..n {
                if !active_bins[j] { continue; }
                
                if bin_size[i] + bin_size[j] <= cacheline {
                    // if strictly 0.0, they have absolutely no interactions
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
                
                for i in 0..n {
                    if bin_of[i] == v {
                        bin_of[i] = u;
                    }
                }
                
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

    // Output formatting
    let mut bins: HashMap<usize, Vec<String>> = HashMap::new();
    for i in 0..n {
        let b = bin_of[i];
        bins.entry(b).or_default().push(nodes[i].name.clone());
    }

    let mut final_groups: Vec<Vec<String>> = bins.into_values().collect();
    final_groups.sort_by_key(|group| group[0].clone());

    use std::io::Write;
    let out_file = dot_file.replace(".dot", ".layout.txt");
    if let Ok(mut f) = File::create(&out_file) {
        for group in final_groups {
            let mut g = group.clone();
            g.sort();
            let _ = writeln!(f, "{}", g.join(" "));
        }
        println!("Output written to {}", out_file);
    } else {
        eprintln!("Error: failed to create output file {}", out_file);
    }
}
