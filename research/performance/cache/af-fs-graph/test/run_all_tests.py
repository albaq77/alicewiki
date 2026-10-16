#!/usr/bin/env python3
import contextlib
import argparse
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import summarize
from pipeline_checks import read_dot, check_array


def linear_index(indices, bounds):
    value = 0
    multiplier = 1
    for index, bound in zip(reversed(indices), reversed(bounds)):
        value += index * multiplier
        multiplier *= bound
    return value


def permuted_index(indices, bounds, selected):
    order = [i for i in range(len(indices)) if i != selected] + [selected]
    return linear_index([indices[i] for i in order], [bounds[i] for i in order])


def layout_offset(indices, bounds, selected, struct_size, field_offset):
    index = permuted_index(indices, bounds, selected)
    return index * struct_size + field_offset


class RegressionTests(unittest.TestCase):
    def generate(self, work, types, variables, trace):
        helper = work / "dbg-fixture"
        helper.write_text(f"#!{sys.executable}\nimport sys\nfrom pathlib import Path\n"
                          f"Path(sys.argv[2]).write_text({types!r})\n"
                          f"Path(sys.argv[3]).write_text({variables!r})\n")
        helper.chmod(0o755)
        (work / "trace.csv").write_text(trace)
        return subprocess.run(
            [str(ROOT / "generator/target/debug/generator"), "-i", str(work / "trace.csv"),
             "-o", str(work / "graph.dot"), "--binary", str(helper),
             "--dbg-viewtype", str(helper), "--window", "30000"],
            check=True, capture_output=True, text=True)

    def test_array_members_preserve_elements_but_pack_each_field_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            self.generate(work, "Entity\t24\n\t0\tvalues\tlong[2]\n\t16\ttail\tlong\n",
                          "entities\t4096\tEntity[2]\ncfg\t8192\tEntity\n",
                          "1,0x1008,W,8,0,10\n2,0x1020,W,8,1,11\n"
                          "3,0x2008,W,8,0,10\n4,0x1000,R,8,0,10\n")
            nodes, _ = read_dot(work / "graph.entities.dot")
            self.assertEqual(set(nodes), {"entities[0].values[0]", "entities[0].values[1]",
                                          "entities[1].values[1]"})
            self.assertEqual(nodes["entities[1].values[1]"]["size"], "8")
            self.assertEqual(nodes["entities[1].values[1]"]["offset"], "8")
            fields, _ = read_dot(work / "graph.Entity.fields.dot")
            self.assertEqual(set(fields), {"Entity::values", "Entity::tail"})
            self.assertEqual(fields["Entity::values"]["size"], "16")
            subprocess.run([str(ROOT / "package_fields/target/debug/package_fields"),
                            str(work / "graph.Entity.fields.dot"), "1", "15", "10", "64"],
                           check=True, capture_output=True)
            import re
            sizes = re.findall(r"placement: offset=\d+ size=(\d+)",
                               (work / "graph.Entity.layout.txt").read_text())
            self.assertEqual(sum(map(int, sizes)), 24)
            subprocess.run([str(ROOT / "summarier/target/debug/summarier"),
                            str(work / "graph.entities.dot"), "1", "15", "10", "64"],
                           check=True, capture_output=True)
            decision = check_array(work / "graph.entities.dot")
            soa = [c for c in decision["candidates"] if c["action"] == "partial_soa"]
            self.assertTrue(soa)
            self.assertTrue(all(c["field"] == ".values" and c["field_stride"] == 16 for c in soa))
            self.assertFalse((work / "graph.cfg.values.dot").exists())

    def test_unknown_padding_and_cross_boundary_accesses_do_not_create_nodes(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            result = self.generate(
                work, "Padded\t16\n\t0\tc\tchar\n\t8\ttail\tlong\n",
                "a\t4096\tlong[2]\np\t8192\tPadded\ng\t12288\tlong\n",
                "1,0x1000,W,8,0,10\n2,0x1010,W,8,1,11\n"  # outside array
                "3,0x1008,W,16,1,11\n4,0x2001,W,1,1,11\n"  # object end / padding
                "5,0x2000,W,16,1,11\n6,0x3008,W,8,1,11\n"  # member end / scalar end
                "7,0x1000,W,0,1,11\n8,0xffffffffffffffff,W,8,1,11\n")
            nodes, edges = read_dot(work / "graph.a.dot")
            self.assertEqual(set(nodes), {"a[0]"})
            self.assertEqual(edges, [])
            self.assertEqual(len(list(work.glob("*.dot"))), 1)
            self.assertIn("could not resolve", result.stderr)

    def test_member_arrays_use_type_stride_and_keep_multidimensional_indices(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            self.generate(work, "Entity\t72\n\t0\tvalues\tlong[2][2]\n\t64\ttail\tlong\n",
                          "cfg\t4096\tEntity\nentities\t8192\tEntity[2]\n",
                          "1,0x1018,W,8,0,10\n2,0x2060,W,8,1,11\n")
            fields, _ = read_dot(work / "graph.Entity.fields.dot")
            self.assertEqual(set(fields), {"Entity::values", "Entity::tail"})
            self.assertEqual(fields["Entity::values"]["size"], "32")
            nodes, _ = read_dot(work / "graph.entities.dot")
            self.assertEqual(set(nodes), {"entities[1].values[1][1]"})
            self.assertEqual(next(iter(nodes.values()))["offset"], "24")

    def test_field_metadata_does_not_replace_access_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            self.generate(work, "Entity\t40\n\t0\tvalues\tlong[4]\n\t32\ttail\tlong\n",
                          "entities\t4096\tEntity[2]\n",
                          "1,0x1018,W,8,0,10\n2,0x1040,W,8,1,11\n")
            dot = work / "graph.entities.dot"
            subprocess.run([str(ROOT / "summarier/target/debug/summarier"), str(dot),
                            "1", "15", "10", "64"], check=True, capture_output=True)
            decision = check_array(dot)
            self.assertEqual(decision["current_score"], 0.1)

    def test_frozen_real_baseline(self):
        from replay_baseline import verify
        self.assertEqual(verify()["status"], "passed")

    def test_legacy_absolute_offsets_remain_usable_for_soa_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            dot = Path(tmp) / "graph.arr.dot"
            dot.write_text('graph G {\n'
                           '"arr[0].a" [type="long", offset=64, size=8, dims="2", elem_size=128];\n'
                           '"arr[1].a" [type="long", offset=192, size=8, dims="2", elem_size=128];\n'
                           '"arr[0].a" -- "arr[1].a" [label="(10,0)"];\n}\n')
            subprocess.run([str(ROOT / "summarier/target/debug/summarier"), str(dot),
                            "1", "15", "10", "64"], check=True, capture_output=True)
            decision = json.loads(dot.with_suffix(".decision.json").read_text())
            soa = [c for c in decision["candidates"] if c["action"] == "partial_soa"]
            self.assertTrue(soa)
            self.assertTrue(all(c["score"] == -150 for c in soa))

    def test_structured_axis_decision_is_authoritative(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "graph.matrix.txt").write_text(
                "dim0 18.5 60 size=16\ndim1 -185.8 48 size=8\n",
                encoding="utf-8",
            )
            (root / "graph.matrix.decision.json").write_text(
                json.dumps({
                    "schema_version": 1,
                    "current_dimension": "dim1",
                    "selected_dimension": "dim0",
                    "action": "axis_permutation",
                    "reason": "regression",
                    "candidates": [],
                }),
                encoding="utf-8",
            )
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                summarize.summarize_arrays(tmp)
            report = output.getvalue()
            self.assertIn("将 dim0 移到最内层", report)
            self.assertNotIn("转为 SOA 架构", report)

    def test_self_loops_preserve_instance_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            dot = Path(tmp) / "graph.Entity.fields.dot"
            dot.write_text(
                '''graph G {
"Entity::shared" -- "Entity::shared" [alpha=7,true_alpha=7,false_alpha=0];
"Entity::health" -- "Entity::health" [alpha=11,true_alpha=0,false_alpha=11];
"Entity::legacy" -- "Entity::legacy" [alpha=3];
"Entity::x" -- "Entity::y" [alpha=99,true_alpha=99,false_alpha=0];
}\n''',
                encoding="utf-8",
            )
            true_edges, false_edges, unknown = summarize.parse_sharing_edges(str(dot))
            self.assertEqual(true_edges, [("shared", 7)])
            self.assertEqual(false_edges, [("health", 11)])
            self.assertEqual(unknown, [("legacy", 3)])

    def test_real_field_offsets_are_not_replaced_by_equal_width_cells(self):
        bounds = [16, 8]
        struct_size = 320
        health = layout_offset([0, 0], bounds, 1, struct_size, 0)
        mana = layout_offset([0, 0], bounds, 1, struct_size, 64)
        next_health = layout_offset([0, 1], bounds, 1, struct_size, 0)
        self.assertEqual((health, mana, next_health), (0, 64, 320))
        self.assertNotEqual(mana, struct_size // 10)
        self.assertNotEqual(health // 64, mana // 64)

    def test_legacy_baseline_metadata_is_preserved(self):
        baseline = json.loads((ROOT / "test" / "baseline.json").read_text(encoding="utf-8"))
        scores = baseline["legacy_scores_before_s4_9"]
        # todo.md says "12/12", but its table contains 2+3+3+3 = 11 scores.
        self.assertEqual(sum(len(item["dimensions"]) for item in scores.values()), 11)
        self.assertEqual(baseline["trace"]["lines"], 55361)
        self.assertEqual(baseline["trace"]["window_ns"], 30000)
        self.assertEqual(set(baseline["geometry_truth"]), set(scores))

    def test_summarier_evolver_selects_full_graph_axis_candidate(self):
        binary = ROOT / "summarier" / "target" / "debug" / "summarier"
        self.assertTrue(binary.exists(), "summarier must be built before E2E")
        with tempfile.TemporaryDirectory() as tmp:
            dot = Path(tmp) / "graph.matrix.dot"
            dot.write_text(
                '''graph G {
"matrix[0][0]" [label="matrix[0][0]", type="long", offset=0, dims="16,8", elem_size=8];
"matrix[1][0]" [label="matrix[1][0]", type="long", offset=0, dims="16,8", elem_size=8];
"matrix[0][1]" [label="matrix[0][1]", type="long", offset=0, dims="16,8", elem_size=8];
"matrix[0][0]" -- "matrix[1][0]" [label="(0,10)"];
"matrix[0][0]" -- "matrix[0][1]" [label="(10,0)"];
}\n''',
                encoding="utf-8",
            )
            subprocess.run([str(binary), str(dot), "1", "15", "10", "64"], check=True,
                           capture_output=True, text=True)
            decision = json.loads((Path(tmp) / "graph.matrix.decision.json").read_text(encoding="utf-8"))
            self.assertEqual(decision["schema_version"], 2)
            self.assertEqual(decision["action"], "axis_permutation")
            self.assertEqual(decision["selected_dimension"], "dim0")
            self.assertGreater(decision["selected_score"], decision["current_score"])

    def test_field_packer_isolates_zero_evidence_node(self):
        binary = ROOT / "package_fields" / "target" / "debug" / "package_fields"
        self.assertTrue(binary.exists(), "package_fields must be built before E2E")
        with tempfile.TemporaryDirectory() as tmp:
            dot = Path(tmp) / "graph.Entity.fields.dot"
            dot.write_text(
                '''graph G {
"Entity::x" [label="Entity::x", type="long", offset=0];
"Entity::y" [label="Entity::y", type="long", offset=8];
"Entity::z" [label="Entity::z", type="long", offset=16];
"Entity::x" -- "Entity::y" [label="(0,100)"];
}\n''',
                encoding="utf-8",
            )
            subprocess.run([str(binary), str(dot), "1", "15", "10", "64"], check=True,
                           capture_output=True, text=True)
            layout = (Path(tmp) / "graph.Entity.layout.txt").read_text(encoding="utf-8")
            self.assertIn("[Cacheline 0 aligned_offset=0 span=1]", layout)
            self.assertIn("[Cacheline 1 aligned_offset=64 span=1]", layout)
            blocks = layout.split("[Cacheline ")[1:]
            self.assertTrue(any("- x" in block and "- y" in block for block in blocks))
            self.assertTrue(any("- z" in block and "- x" not in block and "- y" not in block for block in blocks))

    def evaluate(self, nodes, edges, bounds):
        with tempfile.TemporaryDirectory() as tmp:
            dot = Path(tmp) / "graph.arr.dot"
            declarations = []
            for name, offset, stride in nodes:
                declarations.append(f'"{name}" [type="long", offset={offset}, size=8, dims="{bounds}", elem_size={stride}];')
            links = [f'"{u}" -- "{v}" [label="({a},{b})"];' for u, v, a, b in edges]
            dot.write_text("graph G {\n" + "\n".join(declarations + links) + "\n}\n")
            subprocess.run([str(ROOT / "summarier/target/debug/summarier"), str(dot), "1", "15", "10", "64"],
                           capture_output=True, text=True, check=True)
            return json.loads(dot.with_suffix(".decision.json").read_text())

    def test_partial_soa_and_axis_permutation_are_evaluated_together(self):
        decision = self.evaluate(
            [("arr[0][0].a", 0, 128), ("arr[1][0].a", 0, 128),
             ("arr[0][1].a", 0, 128), ("arr[0][0].b", 64, 128)],
            [("arr[0][0].a", "arr[1][0].a", 0, 1000),
             ("arr[0][0].a", "arr[0][1].a", 100, 0),
             ("arr[0][0].b", "arr[0][0].b", 0, 1)], "8,4")
        self.assertEqual(decision["action"], "partial_soa")
        self.assertEqual(decision["selected_field"], ".a")
        self.assertEqual(decision["axis_order"], [1, 0])
        self.assertEqual(decision["field_stride"], 8)  # Not the 64-byte padded field gap.
        self.assertTrue(all(c["field_stride"] == 8 for c in decision["candidates"] if c["action"] == "partial_soa"))

    def test_axis_can_move_to_outermost_in_three_dimensions(self):
        decision = self.evaluate(
            [("arr[0][0][0]", 0, 8), ("arr[1][0][0]", 0, 8)],
            [("arr[0][0][0]", "arr[1][0][0]", 0, 10)], "2,2,2")
        self.assertIn([1, 0, 2], [c["axis_order"] for c in decision["candidates"]])

    def test_padding_separates_different_instances(self):
        decision = self.evaluate(
            [("arr[0].a", 0, 24), ("arr[1].a", 0, 24), ("arr[0].b", 8, 24)],
            [("arr[0].a", "arr[1].a", 10, 0)], "2")
        self.assertEqual(decision["action"], "instance_padding")
        self.assertEqual(decision["element_stride"], 64)

    def test_generator_preserves_provenance_and_closed_subgraphs(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            types = "Entity\t16\n\t0\thealth\tlong\n\t8\tcold\tlong\nConfig\t16\n\t0\tshared\tlong\n\t8\tflag\tlong\n"
            variables = "entities\t4096\tEntity[2]\ncfg\t8192\tConfig\ng\t12288\tlong\n"
            helper = work / "dbg-fixture"
            helper.write_text(f"#!{sys.executable}\nimport sys\nfrom pathlib import Path\n"
                              f"Path(sys.argv[2]).write_text({types!r})\nPath(sys.argv[3]).write_text({variables!r})\n")
            helper.chmod(0o755)
            trace = work / "trace.csv"
            trace.write_text("1,0x1000,W,8,0,10\n2,0x1010,W,8,1,11\n"
                             "3,0x1018,R,8,1,11\n4,0x2000,R,8,0,10\n"
                             "5,0x2000,W,8,1,11\n6,0x3000,W,8,0,10\n")
            command = [str(ROOT / "generator/target/debug/generator"), "-i", str(trace), "-o", str(work / "graph.dot"),
                       "--binary", str(helper), "--dbg-viewtype", str(helper), "--window", "30000"]
            subprocess.run(command, check=True, capture_output=True, text=True)
            nodes, edges = read_dot(work / "graph.Entity.fields.dot")
            self.assertEqual(set(nodes), {"Entity::health", "Entity::cold"})
            loop = next(e for e in edges if e[0] == e[1] == "Entity::health")
            self.assertEqual((loop[4]["true_alpha"], loop[4]["false_alpha"]), ("0", "1"))
            _, edges = read_dot(work / "graph.Config.fields.dot")
            loop = next(e for e in edges if e[0] == e[1] == "Config::shared")
            self.assertEqual((loop[4]["true_alpha"], loop[4]["false_alpha"]), ("1", "0"))
            globals_, _ = read_dot(work / "graph.dot")
            self.assertEqual(set(globals_), {"g"})
            snapshots = {p.name: p.read_text() for p in work.glob("*.dot")}
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(snapshots, {p.name: p.read_text() for p in work.glob("*.dot")})

    def test_generator_rejects_missing_dwarf_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [str(ROOT / "generator/target/debug/generator"), "-i", str(Path(tmp) / "trace.csv"),
                 "-o", str(Path(tmp) / "graph.dot"), "--binary", "missing",
                 "--dbg-viewtype", str(Path(tmp) / "nonexistent")],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("failed to execute dbg-viewtype", result.stderr)

    def test_packers_honor_scalar_alignment_and_full_array_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            dot = Path(tmp) / "graph.Entity.fields.dot"
            dot.write_text('graph G {\n'
                           '"Entity::a" [type="char", offset=0];\n'
                           '"Entity::b" [type="long", offset=8];\n'
                           '"Entity::padding" [type="volatile long int[10]", offset=16];\n'
                           '"Entity::a" -- "Entity::b" [label="(0,10)"];\n}\n')
            subprocess.run([str(ROOT / "package_fields/target/debug/package_fields"), str(dot),
                            "1", "15", "10", "64"], check=True, capture_output=True, text=True)
            layout = dot.with_name("graph.Entity.layout.txt").read_text()
            self.assertIn("- b\n    placement: offset=0 size=8", layout)
            self.assertIn("- a\n    placement: offset=8 size=1", layout)
            self.assertIn("size=80", layout)
            self.assertIn("span=2", layout)

    def test_placement_file_is_not_an_array_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "graph.placement.txt").write_text("[Cacheline 0 aligned_offset=0]\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                summarize.summarize_arrays(tmp)
            self.assertNotIn("无法解析", output.getvalue())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--real", action="store_true", help="also rebuild and run the real LLVM/C++ benchmark")
    parser.add_argument("--output", type=Path, help="new directory for retained real E2E artifacts")
    parser.add_argument("--real-runs", type=int, default=1,
                        help="bounded real sampling loop; each run checks formulas, never cross-run scores")
    args = parser.parse_args()
    if args.real_runs < 1 or (args.real_runs != 1 and not args.real):
        parser.error("--real-runs must be positive and requires --real")
    cargo = shutil.which("cargo") or "/root/.cargo/bin/cargo"
    for crate in ("generator", "summarier", "package_fields", "package_simple"):
        for action in ("test", "build"):
            subprocess.run([cargo, action, "--manifest-path", str(ROOT / crate / "Cargo.toml"), "--offline"], check=True)
    result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False).result
    if not result.wasSuccessful() or result.skipped:
        sys.exit(1)
    if args.real:
        loop_root = None
        if args.real_runs > 1:
            loop_root = args.output or Path(tempfile.mkdtemp(prefix="af-fs-test-loop-"))
            loop_root.mkdir(parents=True, exist_ok=True)
            if any(loop_root.iterdir()):
                parser.error("--output must be empty")
        summary = {"status": "running", "python_tests": result.testsRun,
                   "requested_real_runs": args.real_runs, "runs": []}
        for index in range(args.real_runs):
            output = loop_root / f"run-{index + 1:02d}" if loop_root else args.output
            command = [sys.executable, str(ROOT / "test/run_real_e2e.py")]
            if output:
                command.extend(["--output", str(output)])
            try:
                subprocess.run(command, check=True)
            except subprocess.CalledProcessError:
                summary.update(status="failed", failed_run=index + 1)
                if loop_root:
                    (loop_root / "loop.json").write_text(json.dumps(summary, indent=2) + "\n")
                raise
            if output:
                summary["runs"].append(json.loads((output / "acceptance.json").read_text()))
        summary["status"] = "passed"
        if loop_root:
            (loop_root / "loop.json").write_text(json.dumps(summary, indent=2) + "\n")
            print(f"Test loop passed: {loop_root / 'loop.json'}")
