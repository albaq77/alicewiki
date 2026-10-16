use regex::Regex;
use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{BufReader, Read, Write};
use std::path::Path;
#[path = "../../type_layout.rs"]
mod type_layout;

#[derive(Debug, Clone)]
struct ParsedNode {
    name: String,
    base: String,
    indices: Vec<u64>,
    suffix: String,
    offset: u64,
    dims: Vec<u64>,
    elem_size: u64,
    byte_size: u64,
    base_address: u64,
    field: String,
    field_offset: u64,
    field_size: u64,
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

struct EvaluatedCandidate {
    name: String,
    action: String,
    dimension: Option<usize>,
    field: Option<String>,
    score: f64,
    edge_count: usize,
    axis_order: Vec<usize>,
    element_stride: u64,
    field_stride: Option<u64>,
}

fn ordered_index(n: &ParsedNode, bounds: &[u64], order: &[usize]) -> u64 {
    order
        .iter()
        .fold(0, |index, &dim| index * bounds[dim] + n.indices[dim])
}

fn candidate_orders(dimensions: usize) -> Vec<Vec<usize>> {
    let current: Vec<usize> = (0..dimensions).collect();
    let mut orders = vec![current.clone()];
    for axis in 0..dimensions {
        let rest: Vec<usize> = current.iter().copied().filter(|&d| d != axis).collect();
        for order in [
            std::iter::once(axis).chain(rest.iter().copied()).collect(),
            rest.into_iter().chain(std::iter::once(axis)).collect(),
        ] {
            if !orders.contains(&order) {
                orders.push(order);
            }
        }
    }
    orders
}

fn score_candidate<F>(
    nodes: &HashMap<String, ParsedNode>,
    edges: &[Edge],
    memoryline: u64,
    locality_gain: f64,
    sharing_cost: f64,
    remote_divisor: f64,
    address: F,
) -> (f64, usize)
where
    F: Fn(&ParsedNode) -> u64,
{
    let mut score = 0.0;
    let mut count = 0usize;
    for edge in edges {
        let (Some(u), Some(v)) = (nodes.get(&edge.u), nodes.get(&edge.v)) else {
            continue;
        };
        let u_addr = address(u);
        let v_addr = address(v);
        let same_line = u_addr / memoryline <= (v_addr + v.byte_size - 1) / memoryline
            && v_addr / memoryline <= (u_addr + u.byte_size - 1) / memoryline;
        score += if same_line {
            locality_gain * edge.b - sharing_cost * edge.a
        } else {
            locality_gain * (edge.a + edge.b) / remote_divisor
        };
        count += 1;
    }
    (score, count)
}

fn align_up(value: u64, alignment: u64) -> u64 {
    if value == 0 {
        0
    } else {
        ((value - 1) / alignment + 1) * alignment
    }
}

fn json_escape(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
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
    if memoryline == 0
        || !gamma_cli.is_finite()
        || gamma_cli <= 0.0
        || !alpha_cli.is_finite()
        || !beta_cli.is_finite()
    {
        eprintln!("Error: invalid scoring parameters");
        std::process::exit(1);
    }

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

    let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[.*?\boffset=(\d+).*\];"#).unwrap();
    // Use multi-line regex for edges
    let edge_re =
        Regex::new(r#""([^"]+)"\s*--\s*"([^"]+)"\s*\[[^\]]*?label="\(([\d\.]+),([\d\.]+)\)""#)
            .unwrap();
    let name_re = Regex::new(r#"^([^\[]+)((?:\[\d+\])+)(.*)$"#).unwrap();
    let dims_re = Regex::new(r#"dims="([0-9,]+)""#).unwrap();
    let elem_re = Regex::new(r#"elem_size=(\d+)"#).unwrap();
    let size_re = Regex::new(r"(?:^|[, ])size=(\d+)").unwrap();
    let base_re = Regex::new(r"base_address=(\d+)").unwrap();
    let field_re = Regex::new(r#"(?:^|[, ])field="([^"]+)""#).unwrap();
    let field_offset_re = Regex::new(r"field_offset=(\d+)").unwrap();
    let field_size_re = Regex::new(r"field_size=(\d+)").unwrap();
    let type_re = Regex::new(r#"type="([^"]+)""#).unwrap();

    let mut nodes: HashMap<String, ParsedNode> = HashMap::new();
    let mut edges = Vec::new();

    for line in file_content.lines() {
        if let Some(caps) = node_re.captures(line) {
            let name = caps[1].to_string();
            let offset: u64 = caps[2].parse().unwrap_or(0);
            let mut dims: Vec<u64> = Vec::new();
            if let Some(dc) = dims_re.captures(line) {
                for token in dc[1].split(',') {
                    if let Ok(value) = token.parse::<u64>() {
                        dims.push(value);
                    }
                }
            }
            let elem_size: u64 = elem_re
                .captures(line)
                .map(|c| c[1].parse::<u64>().unwrap_or(0))
                .unwrap_or(0);
            let byte_size = size_re
                .captures(line)
                .and_then(|c| c[1].parse().ok())
                .or_else(|| {
                    type_re
                        .captures(line)
                        .and_then(|c| type_layout::type_layout(&c[1]).map(|l| l.0))
                })
                .unwrap_or(0);
            let base_address = base_re
                .captures(line)
                .and_then(|c| c[1].parse().ok())
                .unwrap_or(0);

            if let Some(name_caps) = name_re.captures(&name) {
                let base = name_caps[1].to_string();
                let brackets = name_caps[2].to_string();
                let suffix = name_caps[3].to_string();
                let field = field_re
                    .captures(line)
                    .map(|c| c[1].to_string())
                    .unwrap_or_else(|| suffix.clone());
                let field_offset = field_offset_re
                    .captures(line)
                    .and_then(|c| c[1].parse().ok())
                    .unwrap_or(offset);
                let field_size = field_size_re
                    .captures(line)
                    .and_then(|c| c[1].parse().ok())
                    .unwrap_or(byte_size);

                let mut indices = Vec::new();
                for part in brackets.split('[') {
                    if let Some(idx_str) = part.strip_suffix(']') {
                        if let Ok(idx) = idx_str.parse::<u64>() {
                            indices.push(idx);
                        }
                    }
                }

                nodes.insert(
                    name.clone(),
                    ParsedNode {
                        name,
                        base,
                        indices,
                        suffix,
                        offset,
                        dims,
                        elem_size,
                        byte_size,
                        base_address,
                        field,
                        field_offset,
                        field_size,
                    },
                );
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
        eprintln!(
            "Warning: '{}' 中没有边 (节点 {} 个), 图本身可能是空的。",
            dot_file,
            nodes.len()
        );
    }
    println!("[edges] {}: parsed {}/{}", dot_file, m_edges, k_edges);

    // Determine the target array base name from the filename
    // e.g., graph.global_2d.dot -> global_2d
    // or aaa.arr.dot -> aaa.arr
    let path = Path::new(dot_file);
    let file_stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
    let target_base = if file_stem.starts_with("graph.") {
        file_stem
            .strip_prefix("graph.")
            .unwrap_or(file_stem)
            .to_string()
    } else {
        file_stem.to_string()
    };

    // Retain only nodes that match the target base
    nodes.retain(|_, n| n.base == target_base);

    if nodes.is_empty() {
        eprintln!(
            "Error: No nodes matched the target base name '{}' derived from filename '{}'.",
            target_base, file_stem
        );
        std::process::exit(1);
    }

    let num_dims = nodes.values().next().unwrap().indices.len();
    let array_dims = num_dims;

    for n in nodes.values() {
        if n.indices.len() != num_dims {
            eprintln!(
                "Error: Inconsistent dimensions among nodes (expected {}, found {} for {}).",
                num_dims,
                n.indices.len(),
                n.name
            );
            std::process::exit(1);
        }
    }

    if num_dims == 0 {
        eprintln!("Error: Number of dimensions is 0. Not an array dot file.");
        std::process::exit(1);
    }

    // Prefer the dimensions declared by the producer. Observed maxima are only a
    // fallback: a run may not touch every index, which silently changes the stride
    // of that dimension and therefore flips same-line verdicts.
    let mut declared: Vec<u64> = Vec::new();
    for n in nodes.values() {
        if n.dims.is_empty() {
            continue;
        }
        if declared.is_empty() {
            declared = n.dims.clone();
        } else if declared != n.dims {
            eprintln!("Error: inconsistent declared dims in '{}'.", dot_file);
            std::process::exit(1);
        }
    }
    let mut bounds = if declared.len() == num_dims {
        declared.clone()
    } else {
        if !declared.is_empty() {
            eprintln!(
                "Warning: declared dims ({}) do not match node dims ({}); using observed maxima.",
                declared.len(),
                num_dims
            );
        }
        let mut observed = vec![0; num_dims];
        for n in nodes.values() {
            for (d, &idx) in n.indices.iter().enumerate() {
                if idx + 1 > observed[d] {
                    observed[d] = idx + 1;
                }
            }
        }
        observed
    };

    let mut struct_size = 0;
    let mut found_size = false;

    // Prefer the element stride declared by the producer. Deriving it from an
    // arbitrary node pair is HashMap-order dependent, and wrong whenever that pair
    // differs in more than the innermost dimension.
    for n in nodes.values() {
        if n.elem_size > 0 {
            struct_size = n.elem_size;
            found_size = true;
            break;
        }
    }

    for n1 in nodes.values() {
        if found_size {
            break;
        }
        for n2 in nodes.values() {
            if n1.suffix == n2.suffix {
                let idx1 = orig_linear_index(&n1.indices, &bounds);
                let idx2 = orig_linear_index(&n2.indices, &bounds);
                if idx1 != idx2 {
                    let diff_offset = if n1.offset > n2.offset {
                        n1.offset - n2.offset
                    } else {
                        n2.offset - n1.offset
                    };
                    let diff_idx = if idx1 > idx2 {
                        idx1 - idx2
                    } else {
                        idx2 - idx1
                    };
                    struct_size = diff_offset / diff_idx;
                    found_size = true;
                    break;
                }
            }
        }
        if found_size {
            break;
        }
    }

    if !found_size {
        struct_size = 1;
    }

    for n in nodes.values_mut() {
        if n.suffix.is_empty() && n.elem_size > 0 {
            n.byte_size = n.elem_size;
        }
        if n.byte_size == 0 {
            eprintln!("Error: missing field size for '{}'", n.name);
            std::process::exit(1);
        }
    }
    let mut suffix_offsets: HashMap<String, u64> = HashMap::new();
    for n in nodes.values_mut() {
        let orig_idx = orig_linear_index(&n.indices, &bounds);
        // New producers emit offsets relative to one array element.  Accept old
        // DOT files too, where offset was relative to the array base.
        let suffix_offset = if n.offset < struct_size {
            n.offset
        } else {
            n.offset.saturating_sub(orig_idx * struct_size)
        };
        n.offset = suffix_offset;
        if n.field_offset >= struct_size {
            n.field_offset = n.field_offset.saturating_sub(orig_idx * struct_size);
        }
        if let Some(previous) = suffix_offsets.insert(n.suffix.clone(), suffix_offset) {
            if previous != suffix_offset {
                eprintln!(
                    "Error: inconsistent relative offset for suffix '{}' in '{}'.",
                    n.suffix, dot_file
                );
                std::process::exit(1);
            }
        }
    }

    let mut unique_suffixes: Vec<String> = suffix_offsets.keys().cloned().collect();
    unique_suffixes.sort_by_key(|s| (suffix_offsets[s], s.clone()));

    let mut num_dims = array_dims;
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
            if !has_struct_dim && u.suffix != v.suffix {
                continue;
            }

            let mut valid = true;
            for d in 0..num_dims {
                if d == dim_k {
                    let diff = if u.indices[d] > v.indices[d] {
                        u.indices[d] - v.indices[d]
                    } else {
                        v.indices[d] - u.indices[d]
                    };
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
                let layout_offset = |n: &ParsedNode| -> u64 {
                    let array_indices = &n.indices[..array_dims];
                    let array_bounds = &bounds[..array_dims];
                    let array_idx = if dim_k < array_dims {
                        hyp_linear_index(array_indices, array_bounds, dim_k)
                    } else {
                        orig_linear_index(array_indices, array_bounds)
                    };
                    let suffix_offset = *suffix_offsets.get(&n.suffix).unwrap_or(&0);
                    n.base_address + array_idx * struct_size + suffix_offset
                };
                let hyp_off_u = layout_offset(u);
                let hyp_off_v = layout_offset(v);

                let same_line = hyp_off_u / memoryline
                    <= (hyp_off_v + v.byte_size - 1) / memoryline
                    && hyp_off_v / memoryline <= (hyp_off_u + u.byte_size - 1) / memoryline;

                let w = if same_line {
                    alpha_cli * edge.b - beta_cli * edge.a
                } else {
                    alpha_cli * (edge.a + edge.b) / gamma_cli
                };
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
            if has_struct_dim && dim_k + 1 == num_dims {
                let _ = writeln!(
                    f,
                    "dim{} {} {} size={} field",
                    dim_k, w_sums[dim_k], pair_counts[dim_k], bounds[dim_k]
                );
            } else {
                let _ = writeln!(
                    f,
                    "dim{} {} {} size={}",
                    dim_k, w_sums[dim_k], pair_counts[dim_k], bounds[dim_k]
                );
            }
        }
    }

    // Evolver: materialise candidate address functions, feed every graph edge
    // through each layout, and choose only a positive improvement over the
    // current layout.  Per-dimension neighbour averages above remain useful
    // diagnostics, but no longer make the final decision.
    let current_dim = num_dims - 1;
    let array_bounds = &bounds[..array_dims];
    let mut candidates: Vec<EvaluatedCandidate> = Vec::new();
    let orders = candidate_orders(array_dims);
    let is_struct_array = nodes.values().any(|n| n.field.starts_with('.'));
    for order in &orders {
        let is_current = order == &orders[0];
        let (score, edge_count) = score_candidate(
            &nodes,
            &edges,
            memoryline,
            alpha_cli,
            beta_cli,
            gamma_cli,
            |n| {
                n.base_address
                    + ordered_index(n, array_bounds, order) * struct_size
                    + suffix_offsets[&n.suffix]
            },
        );
        candidates.push(EvaluatedCandidate {
            name: format!("axes:{:?}", order),
            action: if is_current {
                "keep"
            } else {
                "axis_permutation"
            }
            .to_string(),
            dimension: Some(if is_current {
                current_dim
            } else {
                *order.last().unwrap()
            }),
            field: None,
            score,
            edge_count,
            axis_order: order.clone(),
            element_stride: struct_size,
            field_stride: None,
        });
    }

    if is_struct_array {
        let padded_stride = align_up(struct_size, memoryline);
        for order in &orders {
            let (score, edge_count) = score_candidate(
                &nodes,
                &edges,
                memoryline,
                alpha_cli,
                beta_cli,
                gamma_cli,
                |n| {
                    align_up(n.base_address, memoryline)
                        + ordered_index(n, array_bounds, order) * padded_stride
                        + *suffix_offsets.get(&n.suffix).unwrap_or(&0)
                },
            );
            candidates.push(EvaluatedCandidate {
                name: format!("instance-padding:{:?}", order),
                action: "instance_padding".to_string(),
                dimension: None,
                field: None,
                score,
                edge_count,
                axis_order: order.clone(),
                element_stride: padded_stride,
                field_stride: None,
            });
        }

        let total_elements: u64 = array_bounds.iter().product();
        let mut field_spans: HashMap<String, u64> = HashMap::new();
        for n in nodes.values() {
            if n.offset < n.field_offset || n.offset - n.field_offset + n.byte_size > n.field_size {
                eprintln!("Error: access outside declared field for '{}'", n.name);
                std::process::exit(1);
            }
            if let Some(previous) = field_spans.insert(n.field.clone(), n.field_size) {
                if previous != n.field_size {
                    eprintln!("Error: inconsistent field size for '{}'", n.field);
                    std::process::exit(1);
                }
            }
        }
        let mut fields: Vec<_> = field_spans.keys().cloned().collect();
        fields.sort();
        for selected_field in &fields {
            let field_span = field_spans[selected_field];
            let aos_base = align_up(field_span.saturating_mul(total_elements), memoryline)
                .saturating_add(memoryline);
            for order in &orders {
                let (score, edge_count) = score_candidate(
                    &nodes,
                    &edges,
                    memoryline,
                    alpha_cli,
                    beta_cli,
                    gamma_cli,
                    |n| {
                        let index = ordered_index(n, array_bounds, order);
                        if &n.field == selected_field {
                            align_up(n.base_address, memoryline)
                                + index * field_span
                                + suffix_offsets[&n.suffix]
                                - n.field_offset
                        } else {
                            align_up(n.base_address, memoryline)
                                + aos_base
                                + index * struct_size
                                + *suffix_offsets.get(&n.suffix).unwrap_or(&0)
                        }
                    },
                );
                candidates.push(EvaluatedCandidate {
                    name: format!("partial-soa:{}:{:?}", selected_field, order),
                    action: "partial_soa".to_string(),
                    dimension: None,
                    field: Some(selected_field.clone()),
                    score,
                    edge_count,
                    axis_order: order.clone(),
                    element_stride: struct_size,
                    field_stride: Some(field_span),
                });
            }
        }
    }

    let current_index = candidates.iter().position(|c| c.action == "keep").unwrap();
    let current_score = candidates[current_index].score;
    let best_index = candidates
        .iter()
        .enumerate()
        .filter(|(_, c)| c.edge_count > 0)
        .max_by(|(_, a), (_, b)| a.score.partial_cmp(&b.score).unwrap())
        .map(|(i, _)| i);
    let selected_index = match best_index {
        Some(i) if candidates[i].score > 0.0 && candidates[i].score > current_score + 1e-9 => i,
        _ => current_index,
    };
    let selected = &candidates[selected_index];
    let (action, reason) = if selected.edge_count == 0 {
        (
            "insufficient_evidence",
            "the target subgraph contains no evaluable edges",
        )
    } else if selected_index == current_index {
        (
            "keep",
            "no evaluated candidate has a positive improvement over the current layout",
        )
    } else {
        (
            selected.action.as_str(),
            "full-graph candidate re-evaluation found a positive improvement",
        )
    };
    let decision_file = dot_file.replace(".dot", ".decision.json");
    if let Ok(mut f) = File::create(&decision_file) {
        let _ = writeln!(f, "{{");
        let _ = writeln!(f, "  \"schema_version\": 2,");
        let _ = writeln!(f, "  \"source\": \"{}\",", json_escape(dot_file));
        let _ = writeln!(f, "  \"current_dimension\": \"dim{}\",", current_dim);
        match selected.dimension {
            Some(d) => {
                let _ = writeln!(f, "  \"selected_dimension\": \"dim{}\",", d);
            }
            None => {
                let _ = writeln!(f, "  \"selected_dimension\": null,");
            }
        }
        match &selected.field {
            Some(field) => {
                let _ = writeln!(f, "  \"selected_field\": \"{}\",", json_escape(field));
            }
            None => {
                let _ = writeln!(f, "  \"selected_field\": null,");
            }
        }
        let _ = writeln!(f, "  \"action\": \"{}\",", action);
        let _ = writeln!(f, "  \"reason\": \"{}\",", reason);
        let _ = writeln!(f, "  \"current_score\": {},", current_score);
        let _ = writeln!(f, "  \"selected_score\": {},", selected.score);
        let _ = writeln!(f, "  \"axis_order\": {:?},", selected.axis_order);
        let _ = writeln!(f, "  \"element_stride\": {},", selected.element_stride);
        let _ = writeln!(
            f,
            "  \"field_stride\": {},",
            selected
                .field_stride
                .map(|s| s.to_string())
                .unwrap_or_else(|| "null".into())
        );
        let _ = writeln!(f, "  \"candidates\": [");
        for (index, candidate) in candidates.iter().enumerate() {
            let comma = if index + 1 == candidates.len() {
                ""
            } else {
                ","
            };
            let dimension = candidate
                .dimension
                .map(|d| format!("\"dim{}\"", d))
                .unwrap_or_else(|| "null".to_string());
            let field = candidate
                .field
                .as_ref()
                .map(|s| format!("\"{}\"", json_escape(s)))
                .unwrap_or_else(|| "null".to_string());
            let _ = writeln!(
                f,
                "    {{\"name\":\"{}\",\"action\":\"{}\",\"dimension\":{},\"field\":{},\"score\":{},\"edges\":{},\"axis_order\":{:?},\"element_stride\":{},\"field_stride\":{}}}{}",
                json_escape(&candidate.name),
                candidate.action,
                dimension,
                field,
                candidate.score,
                candidate.edge_count,
                candidate.axis_order,
                candidate.element_stride,
                candidate
                    .field_stride
                    .map(|s| s.to_string())
                    .unwrap_or_else(|| "null".into()),
                comma
            );
        }
        let _ = writeln!(f, "  ]");
        let _ = writeln!(f, "}}");
    } else {
        eprintln!(
            "Error: failed to create structured decision '{}'.",
            decision_file
        );
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn test_regex() {
        let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[.*?\boffset=(\d+).*\];"#).unwrap();
        let text = "  \"global_2d[0][0]\" [label=\"global_2d[0][0]\", type=\"volatile long[][]\", offset=16];";
        assert!(node_re.is_match(text));

        let name_re = Regex::new(r#"^([^\[]+)((?:\[\d+\])+)(.*)$"#).unwrap();
        let name = "global_2d[0][0]";
        assert!(name_re.is_match(name));
    }
}
