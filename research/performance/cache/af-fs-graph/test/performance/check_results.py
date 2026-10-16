#!/usr/bin/env python3
"""Check measured state against closed-form workload results, independently of C++."""
import argparse
import json
import math
from pathlib import Path
import statistics

CASES = ("matrix_2d", "tensor_3d", "entity_arr", "weapon_arr", "globals", "combined")


def expected_checksum(case, iterations):
    # The fixture has N=16, M=8, P=4; four workers touch columns 0..3.
    warm = 512
    total = warm + iterations
    even_warm = (warm + 1) // 2
    even_run = (iterations + 1) // 2
    even = even_warm + even_run
    fifth = (warm + 4) // 5 + (iterations + 4) // 5
    health = warm * (warm + 1) // 2 + iterations * (iterations + 1) // 2
    pos_sum = (even_warm * (even_warm + 1) // 2 + even_run * even_warm
               + even_run * (even_run + 1) // 2)
    mask = (1 << 64) - 1
    value = 1469598103934665603

    def add(item):
        nonlocal value
        value = ((value ^ (item & mask)) * 1099511628211) & mask

    for _ in range(16):
        for thread in range(8):
            entity = case in ("entity_arr", "combined") and thread < 4
            weapon = case in ("weapon_arr", "combined") and thread < 4
            fields = ([health, -total, thread * even, thread * pos_sum, even, even,
                       10 * fifth, fifth, 5 * fifth, fifth] if entity else [0] * 10)
            fields += ([5 * total, -total, even, 2 * even, 1, fifth] if weapon else [0] * 6)
            fields += [total if case in ("matrix_2d", "combined") and thread < 4 else 0]
            fields += [total if case in ("tensor_3d", "combined") and thread < 4 else 0] * 4
            for field in fields:
                add(field)
    for _ in range(16):
        add(total if case in ("globals", "combined") else 0)
    return str(value)


def check(path):
    results = json.loads(path.read_text())
    if results["status"] != "passed" or set(results["cases"]) != set(CASES):
        raise ValueError("incomplete measurement")
    count = 0
    for case, record in results["cases"].items():
        samples = record["samples"]
        if len(samples) < 5:
            raise ValueError(f"{case}: too few samples")
        expected = expected_checksum(case, record["iterations"])
        if record["checksum"] != expected:
            raise ValueError(f"{case}: recorded state differs from closed-form result")
        for index, pair in enumerate(samples):
            order = ["original", "rewritten"] if index % 2 == 0 else ["rewritten", "original"]
            if pair["order"] != order:
                raise ValueError(f"{case}: nonalternating order")
            for variant in order:
                sample = pair[variant]
                if sample["checksum"] != expected or sample["cpus"] != results["metadata"]["cpus"]:
                    raise ValueError(f"{case}: invalid state or affinity")
                if not math.isfinite(sample["seconds"]) or sample["seconds"] <= 0:
                    raise ValueError(f"{case}: invalid duration")
                count += 1
            ratio = pair["original"]["seconds"] / pair["rewritten"]["seconds"]
            if not math.isclose(pair["speedup"], ratio, rel_tol=1e-12):
                raise ValueError(f"{case}: invalid paired ratio")
        for variant in ("original", "rewritten"):
            median = statistics.median(p[variant]["seconds"] for p in samples) * 1000
            if not math.isclose(record[f"{variant}_median_ms"], median, rel_tol=1e-12):
                raise ValueError(f"{case}: invalid median")
        if not math.isclose(record["paired_speedup_median"], statistics.median(p["speedup"] for p in samples), rel_tol=1e-12):
            raise ValueError(f"{case}: invalid median ratio")
    print(f"PASS: {len(CASES)} cases, {count} timed states match independent closed-form results: {path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    for result in parser.parse_args().results:
        check(result)
