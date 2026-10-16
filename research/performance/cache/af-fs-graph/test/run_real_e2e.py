#!/usr/bin/env python3
"""Build and run the actual C++ -> LLVM -> CSV -> DOT -> report pipeline."""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
import hashlib
import shlex
from pathlib import Path

from pipeline_checks import check_real_run, check_member_run

ROOT = Path(__file__).resolve().parents[1]
WIKI = ROOT.parents[3]
BENCHMARK = ROOT.parents[1] / "benchmark"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, help="new/empty directory for retained artifacts")
    parser.add_argument("--llvm-root", type=Path, default=Path(os.environ.get("LLVM_ROOT", "/usr/local/llvm-19.1.7")),
                        help="one LLVM installation providing both tools and lib/cmake/llvm")
    parser.add_argument("--check-only", type=Path, help="independently recheck retained pipeline artifacts")
    args = parser.parse_args()
    if args.check_only:
        result = check_real_run(args.check_only)
        if (args.check_only / "members").is_dir():
            result["members"] = check_member_run(args.check_only / "members")
        print(json.dumps(result, indent=2))
        return
    directory = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="af-fs-e2e-"))
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.iterdir()):
        parser.error("--output must be empty (existing benchmark files are never cleaned)")
    print(f"E2E artifacts: {directory}", flush=True)
    environment = os.environ.copy()
    cargo_dir = Path("/root/.cargo/bin")
    llvm_root = args.llvm_root.resolve()
    environment["PATH"] = os.pathsep.join([str(llvm_root / "bin"), str(cargo_dir), environment["PATH"]])
    # Build DWARF helper into this run's directory, not into the sibling source tree.
    helper_src = WIKI / "research/debugging/dbg-viewtype"
    commands = [
        ["cmake", "-S", str(ROOT / "field_analysis"), "-B", str(directory / "llvm-build"),
         f"-DLLVM_DIR={llvm_root / 'lib/cmake/llvm'}"],
        ["cmake", "--build", str(directory / "llvm-build"), "-j2"],
        ["cmake", "-S", str(helper_src), "-B", str(directory / "dbg-build")],
        ["cmake", "--build", str(directory / "dbg-build"), "-j2"],
    ]
    for crate in ("generator", "summarier", "package_fields", "package_simple"):
        commands.insert(0, [str(cargo_dir / "cargo"), "build", "--offline", "--manifest-path",
                            str(ROOT / crate / "Cargo.toml")])
    shutil.copy2(BENCHMARK / "Makefile", directory / "Makefile")
    shutil.copy2(BENCHMARK / "test_fields2.cpp", directory / "test_fields2.cpp")
    commands.append([
        "make", f"awiki_path={WIKI}", "SRCS=test_fields2.cpp",
        f"PASS_SO={directory / 'llvm-build/FieldAnalysis.so'}",
        f"RUNTIME_LIB={directory / 'llvm-build/runtime/libaffinity.a'}",
        f"DBG_VIEWTYPE={directory / 'dbg-build/dbg-viewtype'}", "TAG=acceptance",
        "run", "opt", "report",
    ])
    member_directory = directory / "members"
    member_directory.mkdir()
    shutil.copy2(BENCHMARK / "Makefile", member_directory / "Makefile")
    shutil.copy2(ROOT / "test/member_arrays.cpp", member_directory / "member_arrays.cpp")
    commands.append([
        "make", "-C", str(member_directory), f"awiki_path={WIKI}", "SRCS=member_arrays.cpp",
        f"PASS_SO={directory / 'llvm-build/FieldAnalysis.so'}",
        f"RUNTIME_LIB={directory / 'llvm-build/runtime/libaffinity.a'}",
        f"DBG_VIEWTYPE={directory / 'dbg-build/dbg-viewtype'}", "TAG=acceptance",
        "run", "opt", "report",
    ])
    with (directory / "pipeline.log").open("w") as log:
        for command in commands:
            command_text = "+ " + shlex.join(command)
            print(command_text, flush=True)
            log.write(command_text + "\n")
            process = subprocess.Popen(command, cwd=directory, env=environment, text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
                log.flush()
            if process.wait():
                raise subprocess.CalledProcessError(process.returncode, command)
    result = check_real_run(directory)
    result["members"] = check_member_run(member_directory)
    result["trace_sha256"] = hashlib.sha256((directory / "trace.csv").read_bytes()).hexdigest()
    result["llvm_root"] = str(llvm_root)
    (directory / "acceptance.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
