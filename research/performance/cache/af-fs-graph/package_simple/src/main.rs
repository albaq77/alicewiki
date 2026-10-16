use regex::Regex;
use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{BufReader, Read};

#[path = "../../type_layout.rs"]
mod type_layout;

fn guess_base_size(type_str: &str) -> u64 {
    type_layout::type_layout(type_str)
        .unwrap_or_else(|| {
            eprintln!("Error: unknown type size: {type_str}");
            std::process::exit(1)
        })
        .0
}

#[derive(Debug, Clone)]
struct Node {
    id: usize,
    name: String,
    size: u64,
    alignment: u64,
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
    let edge_re =
        Regex::new(r#""([^"]+)"\s*--\s*"([^"]+)"\s*\[[^\]]*?label="\(([\d\.]+),([\d\.]+)\)""#)
            .unwrap();

    let mut nodes_map: HashMap<String, Node> = HashMap::new();
    let mut nodes: Vec<Node> = Vec::new();

    for line in file_content.lines() {
        if let Some(caps) = node_re.captures(line) {
            let name = caps[1].to_string();
            let typ = caps[2].to_string();

            if name.contains('[') {
                continue; // Skip arrays if they slipped in
            }

            let metadata = Regex::new(r"(?:^|[, ])size=(\d+)").unwrap();
            let size = metadata
                .captures(line)
                .and_then(|c| c[1].parse::<u64>().ok())
                .filter(|&s| s > 0)
                .unwrap_or_else(|| guess_base_size(&typ));
            let alignment = type_layout::type_layout(&typ)
                .map(|l| l.1)
                .unwrap_or_else(|| (1u64 << size.trailing_zeros()).min(cacheline));
            let id = nodes.len();
            let node = Node {
                id,
                name: name.clone(),
                size,
                alignment,
            };
            nodes_map.insert(name, node.clone());
            nodes.push(node);
        }
    }

    if nodes.is_empty() {
        eprintln!("Error: No single-variable nodes found in the dot file.");
        std::process::exit(1);
    }

    for node in &nodes {
        if node.size > cacheline {
            eprintln!(
                "Error: variable '{}' occupies {} bytes, larger than the {}-byte cacheline; it cannot be packed as one node.",
                node.name, node.size, cacheline
            );
            std::process::exit(1);
        }
    }

    let n = nodes.len();

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
        eprintln!(
            "Warning: '{}' 中没有边 (节点 {} 个), 图本身可能是空的。",
            dot_file, n
        );
    }
    println!("[edges] {}: parsed {}/{}", dot_file, m_edges, k_edges);

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
            if !active_bins[i] {
                continue;
            }
            for j in (i + 1)..n {
                if !active_bins[j] {
                    continue;
                }

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
        let placement_file = dot_file.replace(".dot", ".placement.txt");
        let mut placement = File::create(&placement_file).ok();
        for (line_id, group) in final_groups.into_iter().enumerate() {
            let mut g = group.clone();
            g.sort_by_key(|name| (std::cmp::Reverse(nodes_map[name].alignment), name.clone()));
            let _ = writeln!(f, "{}", g.join(" "));
            if let Some(ref mut pf) = placement {
                let line_start = line_id as u64 * cacheline;
                let mut offset = line_start;
                let _ = writeln!(pf, "[Cacheline {} aligned_offset={}]", line_id, line_start);
                for name in &g {
                    let size = nodes_map[name].size;
                    let _ = writeln!(pf, "  - {} offset={} size={}", name, offset, size);
                    offset += size;
                }
                let _ = writeln!(
                    pf,
                    "    used={} padding={}",
                    offset - line_start,
                    cacheline - (offset - line_start)
                );
            }
        }
        println!("Output written to {}", out_file);
    } else {
        eprintln!("Error: failed to create output file {}", out_file);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn qualified_arrays_keep_their_full_size() {
        assert_eq!(guess_base_size("volatile int[10]"), 40);
        assert_eq!(guess_base_size("const unsigned long long[2]"), 16);
    }
}
