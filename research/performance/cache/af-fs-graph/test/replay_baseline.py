#!/usr/bin/env python3
"""Replay a frozen real trace and captured DWARF; never refresh expectations."""
import gzip
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from pipeline_checks import check_real_run

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/current-baseline"


def snapshot(work):
    result = check_real_run(work)
    result["outputs_sha256"] = {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(work.glob("graph.*")) if p.suffix in (".dot", ".txt")
    }
    result["decisions"] = {}
    for p in sorted(work.glob("*.decision.json")):
        decision = json.loads(p.read_text())
        decision.pop("source")  # Temporary replay directory is not model input.
        result["decisions"][p.name] = decision
    return result


def replay(work, fixture=FIXTURE):
    raw = gzip.decompress((fixture / "trace.csv.gz").read_bytes())
    (work / "trace.csv").write_bytes(raw)
    # Captured DWARF is part of this fixture, rather than depending on an old ELF.
    helper = work / "captured-dwarf"
    helper.write_text(f"#!{sys.executable}\nimport shutil,sys\n"
                      f"shutil.copyfile({str(fixture / 'types.txt')!r},sys.argv[2])\n"
                      f"shutil.copyfile({str(fixture / 'vars.txt')!r},sys.argv[3])\n")
    helper.chmod(0o755)
    (work / "captured.exe").touch()
    command = [str(ROOT / "generator/target/debug/generator"), "-i", str(work / "trace.csv"),
                 "-o", str(work / "graph.dot"), "--binary", str(work / "captured.exe"),
                 "--dbg-viewtype", str(helper), "--window", "30000"]
    def run(command):
        subprocess.run(command, check=True, capture_output=True, text=True)
    run(command)
    for dot in sorted(work.glob("*.dot")):
        tool = "package_fields" if dot.name.endswith(".fields.dot") else (
            "package_simple" if dot.name == "graph.dot" else "summarier")
        run([str(ROOT / tool / "target/debug" / tool), str(dot), "1", "15", "10", "64"])
    report = subprocess.run([sys.executable, str(ROOT / "summarize.py"), str(work)],
                            env={**os.environ, "DBG_VIEWTYPE": str(helper)},
                            check=True, capture_output=True, text=True)
    (work / "reports").mkdir()
    (work / "reports/acceptance.txt").write_text(report.stdout)
    return snapshot(work)


def verify():
    expected = json.loads((FIXTURE / "expected.json").read_text())
    for name, digest in expected["inputs_sha256"].items():
        assert hashlib.sha256((FIXTURE / name).read_bytes()).hexdigest() == digest, name
    with tempfile.TemporaryDirectory(prefix="af-fs-baseline-") as tmp:
        actual = replay(Path(tmp))
    assert actual == expected["snapshot"], "frozen baseline changed; inspect the regression, do not auto-refresh"
    return {"status": "passed", "trace_rows": actual["trace_rows"],
            "diagnostics": 11, "candidates": sum(a["candidates"] for a in actual["arrays"].values())}


if __name__ == "__main__":
    print(json.dumps(verify(), indent=2))
