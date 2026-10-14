use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader, Read, Write};
use std::collections::HashMap;
use std::path::Path;
use regex::Regex;

#[derive(Debug, Clone)]
struct ParsedNode {
    name: String,
    base: String,
    indices: Vec<u64>,
    suffix: String,
    offset: u64,
}

fn orig_linear_index(indices: &[u64], bounds: &[u64]) -> u64 {
    let mut idx = 0;
    let mut multiplier = 1;
    for (i, b) in indices.iter().rev().zip(bounds.iter().rev()) {
        idx += i * multiplier;
        multiplier *= b;
    }
    idx
}

fn hyp_linear_index(indices: &[u64], bounds: &[u64], dim_k: usize) -> u64 {
    let mut perm_indices = Vec::new();
    let mut perm_bounds = Vec::new();
    for d in 0..indices.len() {
        if d != dim_k {
            perm_indices.push(indices[d]);
            perm_bounds.push(bounds[d]);
        }
    }
    perm_indices.push(indices[dim_k]);
    perm_bounds.push(bounds[dim_k]);
    orig_linear_index(&perm_indices, &perm_bounds)
}

struct Edge {
    u: String,
    v: String,
    a: f64,
    b: f64,
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 6 {
        eprintln!("Usage: summarier <dot_file> <alpha> <beta> <gamma> <memoryline>");
        std::process::exit(1);
    }
    
    let dot_file = &args[1];
    let alpha_cli: f64 = args[2].parse().unwrap_or(1.0);
    let beta_cli: f64 = args[3].parse().unwrap_or(1.0);
    let gamma_cli: f64 = args[4].parse().unwrap_or(1.0);
    let memoryline: u64 = args[5].parse().unwrap_or(64);

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

    let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[.*offset=(\d+).*\];"#).unwrap();
    // Use multi-line regex for edges
    let edge_re = Regex::new(r#""([^"]+)"\s*--\s*"([^"]+)"\s*\[[^\]]*?label="\(([\d\.]+),([\d\.]+)\)""#).unwrap();
    let name_re = Regex::new(r#"^([^\[]+)((?:\[\d+\])+)(.*)$"#).unwrap();

    let mut nodes: HashMap<String, ParsedNode> = HashMap::new();
    let mut edges = Vec::new();

    for line in file_content.lines() {
        if let Some(caps) = node_re.captures(line) {
            let name = caps[1].to_string();
            let offset: u64 = caps[2].parse().unwrap_or(0);
            
            if let Some(name_caps) = name_re.captures(&name) {
                let base = name_caps[1].to_string();
                let brackets = name_caps[2].to_string();
                let suffix = name_caps[3].to_string();
                
                let mut indices = Vec::new();
                for part in brackets.split('[') {
                    if let Some(idx_str) = part.strip_suffix(']') {
                        if let Ok(idx) = idx_str.parse::<u64>() {
                            indices.push(idx);
                        }
                    }
                }
                
                nodes.insert(name.clone(), ParsedNode {
                    name,
                    base,
                    indices,
                    suffix,
                    offset,
                });
            }
        }
    }
    
    for caps in edge_re.captures_iter(&file_content) {
        let u = caps[1].to_string();
        let v = caps[2].to_string();
        let a: f64 = caps[3].parse().unwrap_or(0.0);
        let b: f64 = caps[4].parse().unwrap_or(0.0);
        edges.push(Edge { u, v, a, b });
    }

    if nodes.is_empty() {
        eprintln!("Error: No array nodes matched in the dot file.");
        std::process::exit(1);
    }

    let edge_head_re = Regex::new(r#"(?m)^\s*"[^"]+"\s*--\s*"[^"]+"\s*\["#).unwrap();
    let k_edges = edge_head_re.find_iter(&file_content).count();
    let m_edges = edges.len();
    if m_edges < k_edges {
        eprintln!(
            "Error: 边读取失败 -- '{}' 中有 {} 条边, 只解析出 {} 条。DOT 属性格式可能已变更 (label 写法或属性顺序)。",
            dot_file, k_edges, m_edges
        );
        std::process::exit(1);
    }
    if k_edges == 0 && nodes.len() > 1 {
        eprintln!("Warning: '{}' 中没有边 (节点 {} 个), 图本身可能是空的。", dot_file, nodes.len());
    }
    println!("[edges] {}: parsed {}/{}", dot_file, m_edges, k_edges);
    
    // Determine the target array base name from the filename
    // e.g., graph.global_2d.dot -> global_2d
    // or aaa.arr.dot -> aaa.arr
    let path = Path::new(dot_file);
    let file_stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
    let target_base = if file_stem.starts_with("graph.") {
        file_stem.strip_prefix("graph.").unwrap_or(file_stem).to_string()
    } else {
        file_stem.to_string()
    };
    
    // Retain only nodes that match the target base
    nodes.retain(|_, n| n.base == target_base);
    
    if nodes.is_empty() {
        eprintln!("Error: No nodes matched the target base name '{}' derived from filename '{}'.", target_base, file_stem);
        std::process::exit(1);
    }

    let first_node = nodes.values().next().unwrap();
    let num_dims = first_node.indices.len();
    
    for n in nodes.values() {
        if n.indices.len() != num_dims {
            eprintln!("Error: Inconsistent dimensions among nodes (expected {}, found {} for {}).", num_dims, n.indices.len(), n.name);
            std::process::exit(1);
        }
    }
    
    if num_dims == 0 {
        eprintln!("Error: Number of dimensions is 0. Not an array dot file.");
        std::process::exit(1);
    }

    let mut bounds = vec![0; num_dims];
    for n in nodes.values() {
        for (d, &idx) in n.indices.iter().enumerate() {
            if idx + 1 > bounds[d] {
                bounds[d] = idx + 1;
            }
        }
    }

    let mut suffix_offsets: HashMap<String, u64> = HashMap::new();
    for n in nodes.values() {
        let orig_idx = orig_linear_index(&n.indices, &bounds);
        if orig_idx == 0 {
            suffix_offsets.insert(n.suffix.clone(), n.offset);
        }
    }
    
    let mut struct_size = 0;
    let mut found_size = false;
    
    for n1 in nodes.values() {
        for n2 in nodes.values() {
            if n1.suffix == n2.suffix {
                let idx1 = orig_linear_index(&n1.indices, &bounds);
                let idx2 = orig_linear_index(&n2.indices, &bounds);
                if idx1 != idx2 {
                    let diff_offset = if n1.offset > n2.offset { n1.offset - n2.offset } else { n2.offset - n1.offset };
                    let diff_idx = if idx1 > idx2 { idx1 - idx2 } else { idx2 - idx1 };
                    struct_size = diff_offset / diff_idx;
                    found_size = true;
                    break;
                }
            }
        }
        if found_size { break; }
    }
    
    if !found_size {
        struct_size = 1;
    }
    
    for n in nodes.values() {
        if !suffix_offsets.contains_key(&n.suffix) {
            let orig_idx = orig_linear_index(&n.indices, &bounds);
            let suff_off = n.offset.saturating_sub(orig_idx * struct_size);
            suffix_offsets.insert(n.suffix.clone(), suff_off);
        }
    }
    
    let mut unique_suffixes: Vec<String> = suffix_offsets.keys().cloned().collect();
    unique_suffixes.sort_by_key(|s| suffix_offsets.get(s).unwrap());
    
    let mut num_dims = first_node.indices.len();
    let has_struct_dim = unique_suffixes.len() > 1;
    
    if has_struct_dim {
        num_dims += 1;
        bounds.push(unique_suffixes.len() as u64);
        
        // Add suffix index to each node's indices
        for n in nodes.values_mut() {
            let suffix_idx = unique_suffixes.iter().position(|s| s == &n.suffix).unwrap() as u64;
            n.indices.push(suffix_idx);
        }
    }
    
    let field_size = if has_struct_dim {
        struct_size / (unique_suffixes.len() as u64)
    } else {
        struct_size
    };

    let mut w_sums = vec![0.0; num_dims];
    let mut pair_counts = vec![0usize; num_dims];

    for dim_k in 0..num_dims {
        let mut dim_w = 0.0;
        let mut dim_n = 0usize;
        
        for edge in &edges {
            let u = match nodes.get(&edge.u) {
                Some(n) => n,
                None => continue,
            };
            let v = match nodes.get(&edge.v) {
                Some(n) => n,
                None => continue,
            };
            
            // If we didn't add a struct dimension, suffixes must still match
            if !has_struct_dim && u.suffix != v.suffix { continue; }
            
            let mut valid = true;
            for d in 0..num_dims {
                if d == dim_k {
                    let diff = if u.indices[d] > v.indices[d] { u.indices[d] - v.indices[d] } else { v.indices[d] - u.indices[d] };
                    if diff != 1 {
                        valid = false;
                        break;
                    }
                } else {
                    if u.indices[d] != v.indices[d] {
                        valid = false;
                        break;
                    }
                }
            }
            
            if valid {
                let hyp_idx_u = hyp_linear_index(&u.indices, &bounds, dim_k);
                let hyp_off_u = if has_struct_dim {
                    hyp_idx_u * field_size
                } else {
                    let suff_off = suffix_offsets.get(&u.suffix).unwrap_or(&0);
                    hyp_idx_u * struct_size + suff_off
                };
                
                let hyp_idx_v = hyp_linear_index(&v.indices, &bounds, dim_k);
                let hyp_off_v = if has_struct_dim {
                    hyp_idx_v * field_size
                } else {
                    let suff_off = suffix_offsets.get(&v.suffix).unwrap_or(&0);
                    hyp_idx_v * struct_size + suff_off
                };
                
                let same_line = (hyp_off_u / memoryline) == (hyp_off_v / memoryline);
                
                let mut w = 0.0;
                if same_line {
                    w = alpha_cli * edge.b - beta_cli * edge.a;
                } else {
                    w = alpha_cli * (edge.a + edge.b) / gamma_cli;
                }
                dim_w += w;
                dim_n += 1;
            }
        }
        pair_counts[dim_k] = dim_n;
        w_sums[dim_k] = if dim_n > 0 { dim_w / dim_n as f64 } else { 0.0 };
    }

    let out_file = dot_file.replace(".dot", ".txt");
    if let Ok(mut f) = File::create(out_file) {
        for dim_k in 0..num_dims {
            let _ = writeln!(f, "dim{} {} {}", dim_k, w_sums[dim_k], pair_counts[dim_k]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn test_regex() {
        let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[.*offset=(\d+).*\];"#).unwrap();
        let text = "  \"global_2d[0][0]\" [label=\"global_2d[0][0]\", type=\"volatile long[][]\", offset=16];";
        assert!(node_re.is_match(text));
        
        let name_re = Regex::new(r#"^([^\[]+)((?:\[\d+\])+)(.*)$"#).unwrap();
        let name = "global_2d[0][0]";
        assert!(name_re.is_match(name));
    }
}
