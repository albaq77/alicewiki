#!/usr/bin/env python3
"""Compile and compare complete test_fields2 sources, retaining all A-D operations."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

from run_performance import confidence_interval, make_config
from test_fields2_sources import HERE, generate, verify_addresses, verify_state


def check_results(path):
    result = json.loads(path.read_text())
    if result['status'] != 'passed' or set(result['cases']) != {'original_10_iterations', 'extended_workload'}:
        raise ValueError('incomplete full-source run')
    count = 0
    for case, record in result['cases'].items():
        if case == 'original_10_iterations' and record['iterations'] != 10:
            raise ValueError('original iteration count not preserved')
        for index, pair in enumerate(record['samples']):
            if pair['order'] != (['baseline', 'rewritten'] if index % 2 == 0 else ['rewritten', 'baseline']):
                raise ValueError('invalid pair order')
            for variant in pair['order']:
                sample = pair[variant]
                raw_bytes = gzip.decompress((path.parent / sample['raw_file']).read_bytes())
                if hashlib.sha256(raw_bytes).hexdigest() != sample['raw_sha256']:
                    raise ValueError('raw sample hash mismatch')
                raw = json.loads(raw_bytes)
                if raw['iterations'] != record['iterations'] or raw['cpus'] != result['metadata']['cpus']:
                    raise ValueError('incorrect work count or CPU binding')
                if not math.isfinite(raw['seconds']) or raw['seconds'] <= 0:
                    raise ValueError('invalid elapsed time')
                if verify_state(raw, result['metadata']) != sample['validation']:
                    raise ValueError('state oracle mismatch')
                verify_addresses(raw, variant, result['metadata'])
                if raw['seconds'] != sample['seconds']:
                    raise ValueError('recorded duration mismatch')
                count += 1
            if pair['baseline']['validation']['deterministic_sha256'] != pair['rewritten']['validation']['deterministic_sha256']:
                raise ValueError('deterministic states differ')
            if not math.isclose(pair['speedup'], pair['baseline']['seconds']/pair['rewritten']['seconds'], rel_tol=1e-12):
                raise ValueError('invalid speedup')
        for variant in ('baseline', 'rewritten'):
            value = statistics.median(p[variant]['seconds'] for p in record['samples'])*1000
            if not math.isclose(record[f'{variant}_median_ms'], value, rel_tol=1e-12):
                raise ValueError('invalid median')
    print(f'PASS: {count} complete-source timed states; exact deterministic fields, health bounds, physical layouts')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--decisions', type=Path, help='default: frozen verified run-02 layout manifest')
    parser.add_argument('--samples', type=int, default=11)
    parser.add_argument('--target-ms', type=float, default=200)
    parser.add_argument('--cpus', help='four distinct allowed CPU IDs')
    parser.add_argument('--compiler', default='/usr/local/llvm-19.1.7/bin/clang++')
    parser.add_argument('--check-only', type=Path)
    args = parser.parse_args()
    if args.check_only:
        check_results(args.check_only.resolve())
        return
    if not args.output or args.samples < 5 or not 1 <= args.target_ms <= 1000:
        parser.error('output required; at least 5 samples and target-ms in [1,1000]')
    allowed = sorted(os.sched_getaffinity(0))
    cpus = list(map(int, args.cpus.split(','))) if args.cpus else allowed[:4]
    if len(cpus) != 4 or len(set(cpus)) != 4 or not set(cpus) <= set(allowed):
        parser.error('four distinct allowed CPUs required')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error('output must be empty')
    layouts = make_config(args.decisions)[1] if args.decisions else None
    sources = output / 'sources'
    metadata = generate(sources, layouts)
    metadata.update({'started_utc': datetime.now(timezone.utc).isoformat(), 'platform': platform.platform(),
                     'cpus': cpus, 'allowed_cpus': allowed, 'samples_per_case': args.samples,
                     'target_ms': args.target_ms, 'compiler': subprocess.check_output([args.compiler, '--version'], text=True),
                     'measurement': 'steady_clock before thread creation to after all joins; excludes output and validation; no in-process warmup',
                     'compile_commands': {}, 'source_sha256': {}})
    for variant in ('original', 'baseline', 'rewritten'):
        source = sources / f'test_fields2.{variant}.cpp'
        command = [args.compiler, '-std=c++20', '-O3', '-DNDEBUG', '-pthread', '-Wno-deprecated-volatile',
                   str(source), '-o', str(output / f'test_fields2.{variant}')]
        build = subprocess.run(command, capture_output=True, text=True)
        (output / f'build-{variant}.log').write_text(build.stdout + build.stderr)
        build.check_returncode()
        metadata['compile_commands'][variant] = command
    for path in sources.iterdir():
        metadata['source_sha256'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ('run_test_fields2.py', 'test_fields2_sources.py'):
        metadata['source_sha256'][name] = hashlib.sha256((HERE/name).read_bytes()).hexdigest()
    cgroup = Path('/sys/fs/cgroup/cpu.max')
    metadata['cpu_max'] = cgroup.read_text().strip() if cgroup.exists() else None
    raw_dir = output / 'raw'
    raw_dir.mkdir()
    results = {'status': 'running', 'metadata': metadata, 'cases': {}, 'calibration': []}
    (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')

    def run(variant, iterations, label):
        begin = time.perf_counter()
        p = subprocess.run([str(output/f'test_fields2.{variant}'), str(iterations), *map(str, cpus)],
                           capture_output=True, text=True, check=True, timeout=120)
        process_seconds = time.perf_counter() - begin
        if p.stderr:
            raise RuntimeError(p.stderr)
        raw = json.loads(p.stdout)
        if raw['iterations'] != iterations or raw['cpus'] != cpus or not math.isfinite(raw['seconds']) or raw['seconds'] <= 0:
            raise ValueError('bad duration, work count or affinity')
        validation = verify_state(raw, metadata)
        verify_addresses(raw, variant, metadata)
        data = p.stdout.encode()
        relative = f'raw/{label}-{variant}.json.gz'
        (output/relative).write_bytes(gzip.compress(data, mtime=0))
        return {'seconds': raw['seconds'], 'process_seconds': process_seconds,
                'raw_file': relative, 'raw_sha256': hashlib.sha256(data).hexdigest(), 'validation': validation}

    def measure_case(case, iterations):
        samples = []
        print(f'{case}: {iterations} iterations/worker, {args.samples} alternating pairs', flush=True)
        for index in range(args.samples):
            order = ['baseline', 'rewritten'] if index % 2 == 0 else ['rewritten', 'baseline']
            pair = {'order': order}
            for variant in order:
                pair[variant] = run(variant, iterations, f'{case}-{index:02d}')
            if pair['baseline']['validation']['deterministic_sha256'] != pair['rewritten']['validation']['deterministic_sha256']:
                raise ValueError('deterministic states differ')
            pair['speedup'] = pair['baseline']['seconds']/pair['rewritten']['seconds']
            samples.append(pair)
        old = statistics.median(p['baseline']['seconds'] for p in samples)*1000
        new = statistics.median(p['rewritten']['seconds'] for p in samples)*1000
        ratio = [p['speedup'] for p in samples]
        record = {'iterations': iterations, 'samples': samples, 'baseline_median_ms': old,
                  'rewritten_median_ms': new, 'ratio_of_medians': old/new,
                  'paired_speedup_median': statistics.median(ratio),
                  'paired_speedup_bootstrap_95ci': confidence_interval(ratio),
                  'time_reduction_pct': (1-new/old)*100}
        results['cases'][case] = record
        (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print(f'  {old:.3f} -> {new:.3f} ms, paired speedup {statistics.median(ratio):.3f}x', flush=True)

    measure_case('original_10_iterations', 10)
    iterations = 20000
    for index in range(3):
        probes = {variant: [run(variant, iterations, f'calibration-{index}-{probe}') for probe in range(3)]
                  for variant in ('baseline', 'rewritten')}
        fastest = min(statistics.median(p['seconds'] for p in group) for group in probes.values())
        results['calibration'].append({'iterations': iterations, 'probes': probes})
        scaled = max(20000, min(2000000, int(iterations*args.target_ms/1000/fastest)))
        if .7*args.target_ms/1000 <= fastest <= 2*args.target_ms/1000 or iterations == scaled:
            break
        iterations = scaled
    measure_case('extended_workload', iterations)
    results['status'] = 'passed'
    (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    try:
        check_results(output/'results.json')
    except Exception as error:
        results['status'] = 'failed'
        results['error'] = str(error)
        (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        raise
    report = ['# test_fields2 完整源码性能对照', '',
              '两版保留原 A-D 全部操作，共同以 atomic_ref 修复 GameConfig 竞争；health 值仍依赖调度。',
              '计时包含线程创建、完整 worker 和 join；不包含结果输出及校验。每个样本独立进程。', '',
              '| 用例 | 每线程迭代 | 基线中位 ms | 改写中位 ms | 耗时下降 | 配对加速比 | 95% bootstrap CI |',
              '|---|---:|---:|---:|---:|---:|---|']
    for name, record in results['cases'].items():
        ci = record['paired_speedup_bootstrap_95ci']
        report.append(f"| {name} | {record['iterations']} | {record['baseline_median_ms']:.3f} | "
                      f"{record['rewritten_median_ms']:.3f} | {record['time_reduction_pct']:.1f}% | "
                      f"{record['paired_speedup_median']:.3f}× | {ci[0]:.3f}–{ci[1]:.3f} |")
    report += ['', '10 轮用例易受启动和调度影响。区间跨 1 时不能确认收益。',
               '全部确定性字段含冷字段/显式 padding 逐值校验；health 仅检查合法调度范围，不要求逐值相等。',
               '源码、公共修复 diff、纯布局 diff、全部原始状态及实际地址随结果保存。',
               '未修改原 benchmark/test_fields2.cpp；原始副本也编译，但不将其非原子数据竞争程序作为可靠加速基线。']
    (output/'report.md').write_text('\n'.join(report)+'\n')
    print(output/'report.md', flush=True)


if __name__ == '__main__':
    main()
