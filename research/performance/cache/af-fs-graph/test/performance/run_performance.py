#!/usr/bin/env python3
"""Compile actual candidate rewrites and measure paired, checksum-checked runtimes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "test"))
from pipeline_checks import read_dot

CASES = ("matrix_2d", "tensor_3d", "entity_arr", "weapon_arr", "globals", "combined")


def make_config(decisions):
    decisions = decisions.resolve()
    lines = ["// Generated from verified layout decisions; model cacheline = 64 bytes."]
    manifest = {}
    for name, macro, axes in (("matrix_2d", "MATRIX", [1, 0]),
                             ("tensor_3d", "TENSOR", [1, 0, 2]),
                             ("entity_arr", "ENTITY", [1, 0]), ("weapon_arr", "WEAPON", [1, 0])):
        decision = json.loads((decisions / f"graph.{name}.decision.json").read_text())
        if decision["axis_order"] != axes:
            raise ValueError(f"unsupported {name} axis order; implement that rewrite before timing")
        nodes, _ = read_dot(decisions / f"graph.{name}.dot")
        base = int(next(iter(nodes.values()))["base_address"])
        lines.append(f"constexpr size_t {macro}_MOD = {base % 64};")
        if name in ("entity_arr", "weapon_arr"):
            if decision["action"] != "partial_soa" or decision["field_stride"] != 8:
                raise ValueError(f"unsupported {name} action/field size")
            field = decision["selected_field"].lstrip(".")
            if not re.fullmatch(r"[a-z_]+", field):
                raise ValueError("invalid member identifier")
            typ = "ComplexEntity" if name == "entity_arr" else "WeaponStats"
            lines.append(f"constexpr size_t {macro}_FIELD = offsetof({typ}, {field});")
        elif decision["action"] != "axis_permutation":
            raise ValueError(f"unsupported action for {name}")
        manifest[name] = {k: decision[k] for k in ("action", "axis_order", "selected_field", "element_stride", "field_stride")}
        manifest[name]["original_base_mod_64"] = base % 64
    nodes, _ = read_dot(decisions / "graph.dot")
    addresses = [int(nodes[f"g_var_{i}"]["base_address"]) + int(nodes[f"g_var_{i}"]["offset"]) for i in range(16)]
    first = min(addresses)
    placement = dict((name, int(offset)) for name, offset, size in re.findall(
        r'- (\S+) offset=(\d+) size=(\d+)', (decisions / "graph.placement.txt").read_text()) if size == "8")
    offsets = [placement[f"g_var_{i}"] for i in range(16)]
    if max(offsets) + 8 > 2048 or max(addresses) - first + 8 + first % 64 > 2048:
        raise ValueError("global layout exceeds benchmark allocation")
    if len(set(offsets)) != 16 or any(v % 8 for v in offsets):
        raise ValueError("invalid global placement")
    lines += [f"constexpr size_t GLOBAL_MOD = {first % 64};",
              "constexpr size_t GLOBAL_OLD[16] = {" + ",".join(str(a-first) for a in addresses) + "};",
              "constexpr size_t GLOBAL_NEW[16] = {" + ",".join(map(str, offsets)) + "};"]
    manifest["globals"] = {"original_offsets": [a-first for a in addresses], "rewritten_offsets": offsets,
                           "original_base_mod_64": first % 64}
    return "\n".join(lines) + "\n", manifest


def confidence_interval(values):
    rng = random.Random(20261016)
    medians = sorted(statistics.median(rng.choices(values, k=len(values))) for _ in range(5000))
    return [medians[125], medians[4874]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decisions", type=Path, required=True, help="verified graph/decision/placement directory")
    parser.add_argument("--output", type=Path, required=True, help="new/empty retained result directory")
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--target-ms", type=float, default=40)
    parser.add_argument("--cpus", help="four distinct allowed CPU IDs, comma separated")
    parser.add_argument("--compiler", default="/usr/local/llvm-19.1.7/bin/clang++")
    args = parser.parse_args()
    if args.samples < 5 or not 1 <= args.target_ms <= 1000:
        parser.error("use at least 5 samples and target-ms in [1,1000]")
    allowed = sorted(os.sched_getaffinity(0))
    cpus = list(map(int, args.cpus.split(","))) if args.cpus else allowed[:4]
    if len(cpus) != 4 or len(set(cpus)) != 4 or not set(cpus) <= set(allowed):
        parser.error("four distinct allowed CPUs required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("output must be empty")
    config, manifest = make_config(args.decisions)
    (output / "layout_config.hpp").write_text(config)
    source = Path(__file__).with_name("layout_bench.cpp")
    binary = output / "layout_bench"
    compile_command = [args.compiler, "-std=c++20", "-O3", "-DNDEBUG", "-pthread",
                       "-Wno-deprecated-volatile", "-Wno-return-type", "-I", str(output), str(source), "-o", str(binary)]
    build = subprocess.run(compile_command, capture_output=True, text=True)
    (output / "build.log").write_text(build.stdout + build.stderr)
    build.check_returncode()
    metadata = {"compiler": subprocess.check_output([args.compiler, "--version"], text=True),
                "compile_command": compile_command, "platform": platform.platform(), "cpus": cpus,
                "allowed_cpus": allowed, "model_line_bytes": 64, "target_ms": args.target_ms,
                "samples_per_case": args.samples, "started_utc": datetime.now(timezone.utc).isoformat(),
                "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "benchmark_source_sha256": hashlib.sha256((ROOT.parents[1] / "benchmark/test_fields2.cpp").read_bytes()).hexdigest(),
                "layouts": manifest,
                "scope": "untraced isolated layout workload derived from test_fields2; racing GameConfig replaced by iter+1 per worker"}
    # Retain the exact recommendation inputs alongside raw timing evidence.
    input_dir = output / "decision_inputs"
    input_dir.mkdir()
    input_hashes = {}
    for pattern in ("graph.*.decision.json", "graph.placement.txt", "graph*.dot"):
        for path in sorted(args.decisions.glob(pattern)):
            input_hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
            if path.suffix != ".dot":
                (input_dir / path.name).write_bytes(path.read_bytes())
    metadata["decision_input_sha256"] = input_hashes
    cgroup = Path("/sys/fs/cgroup/cpu.max")
    if cgroup.exists():
        metadata["cpu_max"] = cgroup.read_text().strip()
    results = {"status": "running", "metadata": metadata, "cases": {}}

    def run(case, variant, iterations):
        command = [str(binary), case, variant, str(iterations), *map(str, cpus)]
        record = json.loads(subprocess.check_output(command, text=True, timeout=120))
        if not math.isfinite(record["seconds"]) or record["seconds"] <= 0 or record["cpus"] != cpus:
            raise RuntimeError("invalid timing or CPU binding")
        return record

    for case in CASES:
        # Extend probes when short startup-scale runs underpredict steady-state speed.
        iterations = 20000
        calibration = []
        for _ in range(3):
            probes = {variant: [run(case, variant, iterations) for _ in range(3)]
                      for variant in ("original", "rewritten")}
            checksums = {p["checksum"] for group in probes.values() for p in group}
            if len(checksums) != 1:
                raise RuntimeError(f"{case}: calibration state mismatch")
            fastest = min(statistics.median(p["seconds"] for p in group) for group in probes.values())
            calibration.append({"iterations": iterations, "probes": probes})
            scaled = max(20000, min(120000000, int(iterations * args.target_ms / 1000 / fastest)))
            if 0.7 * args.target_ms / 1000 <= fastest <= 2 * args.target_ms / 1000 or scaled == iterations:
                break
            iterations = scaled
        samples = []
        expected_checksum = None
        print(f"{case}: {iterations} iterations, {args.samples} alternating pairs", flush=True)
        for index in range(args.samples):
            pair = {"order": ["original", "rewritten"] if index % 2 == 0 else ["rewritten", "original"]}
            for variant in pair["order"]:
                pair[variant] = run(case, variant, iterations)
            checksum = pair["original"]["checksum"]
            if checksum != pair["rewritten"]["checksum"] or (expected_checksum is not None and checksum != expected_checksum):
                raise RuntimeError(f"{case}: logical state mismatch")
            expected_checksum = checksum
            pair["speedup"] = pair["original"]["seconds"] / pair["rewritten"]["seconds"]
            samples.append(pair)
        original = [p["original"]["seconds"] for p in samples]
        rewritten = [p["rewritten"]["seconds"] for p in samples]
        ratios = [p["speedup"] for p in samples]
        result = {"iterations": iterations, "calibration": calibration, "samples": samples,
                  "original_median_ms": statistics.median(original) * 1000,
                  "rewritten_median_ms": statistics.median(rewritten) * 1000,
                  "paired_speedup_median": statistics.median(ratios),
                  "paired_speedup_bootstrap_95ci": confidence_interval(ratios),
                  "time_reduction_pct": (1-statistics.median(rewritten)/statistics.median(original))*100,
                  "original_range_ms": [min(original)*1000, max(original)*1000],
                  "rewritten_range_ms": [min(rewritten)*1000, max(rewritten)*1000],
                  "checksum": expected_checksum}
        results["cases"][case] = result
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"  {result['original_median_ms']:.3f} -> {result['rewritten_median_ms']:.3f} ms, "
              f"paired speedup {result['paired_speedup_median']:.3f}x", flush=True)
    results["status"] = "passed"
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    report = ["# 布局改写性能实测", "", f"同一未插桩 -O3 二进制，4 个固定 CPU，每项 {args.samples} 对交替样本；启动/分配在计时外，512 次预热。",
              "GameConfig 的竞争读写以 iter+1 线程本地输入替代；这是布局访问模式实测，不是原有数据竞争程序的完整业务计时。",
              "每对样本及跨样本逻辑状态 checksum 一致；保留所有原始计时，不剔除异常值。", "",
              "| 用例 | 原布局中位 ms | 改写中位 ms | 配对加速比中位 | 95% bootstrap CI | 耗时下降 |",
              "|---|---:|---:|---:|---|---:|"]
    for case, r in results["cases"].items():
        ci = r["paired_speedup_bootstrap_95ci"]
        report.append(f"| {case} | {r['original_median_ms']:.3f} | {r['rewritten_median_ms']:.3f} | "
                      f"{r['paired_speedup_median']:.3f}× | {ci[0]:.3f}–{ci[1]:.3f} | {r['time_reduction_pct']:.1f}% |")
    report += ["", "CI 来自有限配对样本的中位数 bootstrap，仅反映本环境的采样波动，不代表跨机器保证。",
               "模型假设 64B 缓存行，宿主物理缓存行及频率在虚拟机中未独立确认。完整参数、布局与原始样本见 results.json。"]
    (output / "report.md").write_text("\n".join(report) + "\n")
    print(output / "report.md", flush=True)


if __name__ == "__main__":
    main()
