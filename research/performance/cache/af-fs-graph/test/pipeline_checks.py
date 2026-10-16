"""Independent checks for real DOT geometry, candidate scores and packing."""
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path


def read_dot(path):
    text = Path(path).read_text(encoding="utf-8")
    nodes = {}
    for name, body in re.findall(r'^\s*"([^"]+)"\s*\[(.*)\];$', text, re.M):
        nodes[name] = dict((m[0], m[1] or m[2]) for m in
                           re.findall(r'(\w+)=(?:"([^"]*)"|([^,\s]+))', body))
    edges = []
    for u, v, body in re.findall(r'"([^"]+)"\s*--\s*"([^"]+)"\s*\[(.*?)\];', text, re.S):
        attrs = dict(re.findall(r'(\w+)=(\d+)', body))
        alpha, beta = map(float, re.search(r'label="\(([\d.]+),([\d.]+)\)"', body).groups())
        edges.append((u, v, alpha, beta, attrs))
    for u, v, a, b, attrs in edges:
        assert u in nodes and v in nodes, (path, u, v, "external endpoint")
        if "count1" in attrs:
            assert a == int(attrs["count1"]) + int(attrs["count2"])
            assert b == int(attrs["count3"]) + int(attrs["count4"])
            assert int(attrs["true_alpha"]) + int(attrs["false_alpha"]) == int(attrs["count1"])
    return nodes, edges


def index_of(name, bounds, order):
    indices = array_indices(name)
    result = 0
    for d in order:
        result = result * bounds[d] + indices[d]
    return result


def array_indices(name):
    prefix = re.match(r'^[^\[]+((?:\[\d+\])+)', name).group(1)
    return list(map(int, re.findall(r'\[(\d+)\]', prefix)))


def suffix_of(name):
    return re.match(r'^[^\[]+(?:\[\d+\])+(.*)$', name).group(1)


def same_line(a, size_a, b, size_b, line=64):
    return a // line <= (b + size_b - 1) // line and b // line <= (a + size_a - 1) // line


def check_array(dot):
    nodes, edges = read_dot(dot)
    decision = json.loads(dot.with_suffix(".decision.json").read_text())
    first = next(iter(nodes.values()))
    bounds = list(map(int, first["dims"].split(",")))
    total = math.prod(bounds)
    for candidate in decision["candidates"]:
        order = candidate["axis_order"]
        assert sorted(order) == list(range(len(bounds)))
        stride = candidate["element_stride"]
        field = candidate["field"]
        field_stride = candidate["field_stride"]
        def address(name):
            n = nodes[name]
            base = int(n["base_address"])
            index = index_of(name, bounds, order)
            offset = int(n["offset"])
            if candidate["action"] == "partial_soa":
                base = (base + 63) // 64 * 64
                if n.get("field", suffix_of(name)) == field:
                    assert int(n.get("field_size", n["size"])) == field_stride
                    return base + index * field_stride + offset - int(n.get("field_offset", offset))
                return base + (field_stride * total + 63) // 64 * 64 + 64 + index * stride + offset
            if candidate["action"] == "instance_padding":
                base = (base + 63) // 64 * 64
                assert stride % 64 == 0
            return base + index * stride + offset
        score = 0.0
        for u, v, a, b, _ in edges:
            local = same_line(address(u), int(nodes[u]["size"]),
                              address(v), int(nodes[v]["size"]))
            score += b - 15 * a if local else (a + b) / 10
        assert math.isclose(score, candidate["score"], rel_tol=1e-10, abs_tol=1e-6), (dot, candidate, score)
        assert candidate["edges"] == len(edges)
    current = next(c for c in decision["candidates"] if c["action"] == "keep")
    assert math.isclose(current["score"], decision["current_score"])
    best = max(c["score"] for c in decision["candidates"])
    if best > 0 and best > current["score"] + 1e-9:
        assert math.isclose(best, decision["selected_score"])
        assert decision["action"] != "keep"
    else:
        assert decision["action"] in ("keep", "insufficient_evidence")
    # Independently recompute every legacy dimension diagnostic, including the
    # field dimension using real offsets instead of equal-width pseudo-fields.
    suffixes = sorted({suffix_of(n) for n in nodes},
                      key=lambda s: next(int(a["offset"]) for n, a in nodes.items() if suffix_of(n) == s))
    for line in dot.with_suffix(".txt").read_text().splitlines():
        dim, expected, pairs, *_ = line.split()
        d = int(dim[3:])
        values = []
        for u, v, a, b, _ in edges:
            iu = array_indices(u)
            iv = array_indices(v)
            if len(suffixes) > 1:
                iu.append(suffixes.index(suffix_of(u)))
                iv.append(suffixes.index(suffix_of(v)))
            if all(abs(x - y) == (1 if axis == d else 0)
                   for axis, (x, y) in enumerate(zip(iu, iv))):
                order = [i for i in range(len(bounds)) if i != d] + ([d] if d < len(bounds) else [])
                def offset(name):
                    n = nodes[name]
                    return int(n["base_address"]) + index_of(name, bounds, order) * int(n["elem_size"]) + int(n["offset"])
                local = same_line(offset(u), int(nodes[u]["size"]), offset(v), int(nodes[v]["size"]))
                values.append(b - 15 * a if local else (a + b) / 10)
        assert len(values) == int(pairs), (dot, dim, "pair count")
        assert math.isclose(sum(values) / len(values) if values else 0, float(expected),
                            rel_tol=1e-10, abs_tol=1e-6), (dot, dim, "diagnostic score")
    return decision


def check_real_run(directory):
    directory = Path(directory)
    owners = defaultdict(set)
    threads, cores = set(), set()
    rows, previous = 0, -1
    with (directory / "trace.csv").open() as trace:
        for row in csv.reader(trace):
            assert len(row) == 6, row
            timestamp, address, mode, size, core, thread = row
            assert int(timestamp) >= previous
            previous = int(timestamp)
            assert mode in ("R", "W") and int(size) > 0
            owners[int(address, 0)].add(int(thread))
            threads.add(int(thread))
            cores.add(int(core))
            rows += 1
    assert rows > 10000 and len(threads) >= 4
    summary = {"trace_rows": rows, "threads": len(threads), "cores": len(cores), "arrays": {}}
    for name in ("matrix_2d", "tensor_3d", "entity_arr", "weapon_arr"):
        dot = directory / f"graph.{name}.dot"
        decision = check_array(dot)
        nodes, _ = read_dot(dot)
        lines = defaultdict(set)
        for label, n in nodes.items():
            assert label.startswith(name + "[")
            bounds = list(map(int, n["dims"].split(",")))
            address = int(n["base_address"]) + index_of(label, bounds, range(len(bounds))) * int(n["elem_size"]) + int(n["offset"])
            assert address in owners, (label, address, "address missing from trace")
            assert int(n["offset"]) < int(n["elem_size"])
            for line in range(address // 64, (address + int(n["size"]) - 1) // 64 + 1):
                lines[line].update(owners[address])
        shared = sum(len(tids) > 1 for tids in lines.values())
        if name in ("entity_arr", "weapon_arr"):
            assert shared == 0, (name, shared)
            true, false = 0, 0
            _, struct_edges = read_dot(directory / ("graph.ComplexEntity.fields.dot" if name == "entity_arr" else "graph.WeaponStats.fields.dot"))
            for u, v, _, _, attrs in struct_edges:
                if u == v:
                    true += int(attrs["true_alpha"])
                    false += int(attrs["false_alpha"])
            assert true == false == 0, (name, true, false)
        summary["arrays"][name] = {"action": decision["action"], "axis_order": decision["axis_order"],
                                  "current_score": decision["current_score"], "selected_score": decision["selected_score"],
                                  "candidates": len(decision["candidates"]), "shared_lines": shared}
    for struct, count in (("ComplexEntity", 14), ("GameConfig", 9), ("WeaponStats", 9)):
        nodes, _ = read_dot(directory / f"graph.{struct}.fields.dot")
        assert len(nodes) == count, (struct, len(nodes))
        assert all(n.startswith(struct + "::") for n in nodes)
        layout = (directory / f"graph.{struct}.layout.txt").read_text()
        placements = re.findall(r'^  - (.+)\n\s+placement: offset=(\d+) size=(\d+)', layout, re.M)
        assert {struct + "::" + p[0] for p in placements} == set(nodes), (struct, "cold fields lost")
        occupied = []
        for field, offset, size in placements:
            offset, size = int(offset), int(size)
            assert size == int(nodes[struct + "::" + field]["size"])
            assert offset % 8 == 0  # All real benchmark fields are long / long arrays.
            assert all(offset + size <= start or offset >= end for start, end in occupied)
            occupied.append((offset, offset + size))
        for line_id, start, span, block in re.findall(
                r'\[Cacheline (\d+) aligned_offset=(\d+) span=(\d+)\]([^\[]*)', layout):
            start, span = int(start), int(span)
            assert start == int(line_id) * 64 and span > 0
            for offset, size in re.findall(r'placement: offset=(\d+) size=(\d+)', block):
                assert start <= int(offset) and int(offset) + int(size) <= start + span * 64
    globals_, _ = read_dot(directory / "graph.dot")
    assert len(globals_) == 16 and all(n.startswith("g_var_") for n in globals_)
    placements = re.findall(r'- (\S+) offset=(\d+) size=(\d+)',
                            (directory / "graph.placement.txt").read_text())
    assert {p[0] for p in placements} == set(globals_)
    new_owners = defaultdict(set)
    for name, offset, size in placements:
        node = globals_[name]
        assert int(size) == int(node["size"]) and int(offset) % 8 == 0
        original_address = int(node["base_address"]) + int(node["offset"])
        new_owners[int(offset) // 64].update(owners[original_address])
    assert all(len(tids) == 1 for tids in new_owners.values()), "packing introduced cross-thread sharing"
    report = (directory / "reports" / "acceptance.txt").read_text()
    assert "❌" not in report and "数组实体: \x1b[92mplacement" not in report
    summary["status"] = "passed"
    return summary


def check_member_run(directory):
    """Check the newly compiled member-array C++ workload against its ABI."""
    directory = Path(directory)
    owners = set()
    with (directory / "trace.csv").open() as trace:
        for row in csv.reader(trace):
            owners.add((int(row[1], 0), int(row[3])))
    for name, stride, count, field_size in (("entities", 40, 10, 32), ("padded", 16, 8, 3)):
        nodes, edges = read_dot(directory / f"graph.{name}.dot")
        assert len(nodes) == count, (name, nodes)
        assert all("+" not in n for n in nodes), (name, "padding or synthetic fields leaked")
        for label, node in nodes.items():
            assert int(node["elem_size"]) == stride
            address = int(node["base_address"]) + array_indices(label)[0] * stride + int(node["offset"])
            assert (address, int(node["size"])) in owners
        decision = check_array(directory / f"graph.{name}.dot")
        soa = [c for c in decision["candidates"] if c["action"] == "partial_soa" and c["field"] == ".values"]
        assert soa and all(c["field_stride"] == field_size for c in soa)
    for typ, sizes in (("MemberEntity", {"values": 32, "tail": 8}),
                       ("PaddedMember", {"values": 3, "tail": 8})):
        fields, _ = read_dot(directory / f"graph.{typ}.fields.dot")
        assert {name.split("::")[1]: int(n["size"]) for name, n in fields.items()} == sizes
        layout = (directory / f"graph.{typ}.layout.txt").read_text()
        placements = re.findall(r'^  - (.+)\n\s+placement: offset=(\d+) size=(\d+)', layout, re.M)
        assert len(placements) == len(sizes)
        assert {p[0]: int(p[2]) for p in placements} == sizes
        for i, (_, offset, size) in enumerate(placements):
            assert all(int(offset) + int(size) <= int(o) or int(offset) >= int(o) + int(s)
                       for _, o, s in placements[:i])
    report = (directory / "reports/acceptance.txt").read_text()
    assert "❌" not in report
    return {"status": "passed", "array_members": 18, "struct_fields": 4}
