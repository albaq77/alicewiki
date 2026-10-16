"""A deliberately scoped source adapter for the checked test_fields2 fixture."""
import difflib
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ORIGINAL = ROOT.parents[1] / 'benchmark/test_fields2.cpp'
FROZEN = HERE / 'fixtures/layout_decisions.json'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'fixture changed: expected one occurrence of {old!r}')
    return text.replace(old, new, 1)


def fields(source, typename):
    body = re.search(r'struct ' + typename + r' \{(.*?)\n\};', source, re.S).group(1)
    result = [(name, int(count or 1)) for name, count in re.findall(
        r'volatile long int (\w+)(?:\[(\d+)\])?;', body)]
    if not result:
        raise ValueError(f'no members for {typename}')
    return result


def generate(output, layouts=None):
    source = ORIGINAL.read_text()
    frozen = json.loads(FROZEN.read_text())
    if hashlib.sha256(ORIGINAL.read_bytes()).hexdigest() != frozen['benchmark_source_sha256']:
        raise ValueError('test_fields2.cpp changed: review the scoped adapter before regenerating')
    layouts = layouts or frozen['layouts']
    for name, action, field, axes in (
            ('matrix_2d', 'axis_permutation', None, [1, 0]),
            ('tensor_3d', 'axis_permutation', None, [1, 0, 2]),
            ('entity_arr', 'partial_soa', '.health', [1, 0]),
            ('weapon_arr', 'partial_soa', '.base_damage', [1, 0])):
        layout = layouts[name]
        if (layout['action'], layout['selected_field'], layout['axis_order']) != (action, field, axes):
            raise ValueError(f'unsupported full-source rewrite for {name}')
    schema = {name: fields(source, name) for name in ('ComplexEntity', 'WeaponStats', 'GameConfig')}
    prefix, rest = source.split('void worker(int thread_id)', 1)
    worker, old_main = ('void worker(int thread_id)' + rest).split('\nint main()', 1)
    if 'return 0;' not in old_main:
        raise ValueError('unexpected original main')
    prefix = replace_once(prefix, 'const int ITERATIONS = 10; // 遵循用户要求：总访问量不要太大',
                          'long ITERATIONS = 10; // Default preserved; measurement CLI can extend both versions equally.')
    prefix = '#include <array>\n#include <atomic>\n#include <cstddef>\n#include <cstdint>\n#include <string>\n#include <utility>\n' + prefix
    prefix += '''
static_assert(N == 16 && M == 8 && P == 4 && NUM_THREADS == 4);
static_assert(sizeof(long) == 8 && sizeof(ComplexEntity) == 320 && sizeof(WeaponStats) == 240);
static_assert(std::atomic_ref<volatile long>::is_always_lock_free);
static_assert(alignof(GameConfig) >= std::atomic_ref<volatile long>::required_alignment);
// Shared safety repair in BOTH variants: retain all original config operations.
static void config_add(volatile long &field, long value) {
    std::atomic_ref<volatile long>(field).fetch_add(value, std::memory_order_relaxed);
}
static long config_load(volatile long &field) {
    return std::atomic_ref<volatile long>(field).load(std::memory_order_relaxed);
}

'''
    for field, value in (('difficulty', '1'), ('max_retries', '1'), ('max_players', 'thread_id'),
                         ('timeout_ms', '10'), ('spawn_rate', '1'), ('drop_rate', '1')):
        worker = replace_once(worker, f'global_cfg.{field} += {value};', f'config_add(global_cfg.{field}, {value});')
    worker = replace_once(worker, '+= global_cfg.difficulty;', '+= config_load(global_cfg.difficulty);')
    snapshot = ['std::vector<long> snapshot() {', '    std::vector<long> out;',
                '    auto add = [&](long value) { out.push_back(value); };',
                '    for (int i = 0; i < N; ++i) for (int t = 0; t < M; ++t) {']
    for obj, typ in (('entity_arr', 'ComplexEntity'), ('weapon_arr', 'WeaponStats')):
        for field, count in schema[typ]:
            if count > 1:
                snapshot.append(f'        for (int k = 0; k < {count}; ++k) add({obj}[i][t].{field}[k]);')
            else:
                snapshot.append(f'        add({obj}[i][t].{field});')
    snapshot += ['        add(matrix_2d[i][t]);', '        for (int k = 0; k < P; ++k) add(tensor_3d[i][t][k]);', '    }']
    for field, count in schema['GameConfig']:
        if count > 1:
            snapshot.append(f'    for (int k = 0; k < {count}; ++k) add(global_cfg.{field}[k]);')
        else:
            snapshot.append(f'    add(config_load(global_cfg.{field}));')
    snapshot += [f'    add(g_var_{i});' for i in range(30)]
    snapshot += ['    return out;', '}\n']
    addresses = '''std::vector<std::pair<std::string, uintptr_t>> runtime_addresses() {
    std::vector<std::pair<std::string, uintptr_t>> out;
    auto add = [&](const char *name, const volatile long &value) {
        out.emplace_back(name, reinterpret_cast<uintptr_t>(&value));
    };
    add("entity_health_0_0", entity_arr[0][0].health);
    add("entity_health_1_0", entity_arr[1][0].health);
    add("entity_health_0_1", entity_arr[0][1].health);
    add("entity_mana_0_0", entity_arr[0][0].mana);
    add("entity_mana_1_0", entity_arr[1][0].mana);
    add("entity_mana_0_1", entity_arr[0][1].mana);
    add("weapon_damage_0_0", weapon_arr[0][0].base_damage);
    add("weapon_damage_1_0", weapon_arr[1][0].base_damage);
    add("weapon_damage_0_1", weapon_arr[0][1].base_damage);
    add("matrix_0_0", matrix_2d[0][0]);
    add("matrix_1_0", matrix_2d[1][0]);
    add("matrix_0_1", matrix_2d[0][1]);
    add("tensor_0_0", tensor_3d[0][0][0]);
    add("tensor_1_0", tensor_3d[1][0][0]);
    add("tensor_0_1", tensor_3d[0][1][0]);
'''
    addresses += ''.join(f'    add("g_var_{i}", g_var_{i});\n' for i in range(16))
    addresses += '    return out;\n}\n'
    body = worker + '\n' + '\n'.join(snapshot) + addresses
    main = '\n#include "test_fields2_support.hpp"\nint main(int argc, char **argv) { return run_test_fields2(argc, argv); }\n'
    baseline = prefix + body + main
    changed_prefix = prefix
    for obj, typ, selected, layout in (('entity_arr', 'ComplexEntity', 'health', 'EntityLayout'),
                                        ('weapon_arr', 'WeaponStats', 'base_damage', 'WeaponLayout')):
        var = 'entity_layout' if obj == 'entity_arr' else 'weapon_layout'
        replacement = f'''struct alignas(64) {layout} {{
    volatile long {selected}[M][N];
    unsigned char soa_gap[64];
    {typ} residual[M][N]; // Preserve original struct stride and selected-member hole.
}};
static_assert(offsetof({layout}, residual) == 1088);
{layout} {var};'''
        changed_prefix = replace_once(changed_prefix, f'{typ} {obj}[N][M];', replacement)
    changed_prefix = replace_once(changed_prefix, 'volatile long int matrix_2d[N][M];',
                                  'volatile long int matrix_2d[M][N];')
    changed_prefix = replace_once(changed_prefix, 'volatile long int tensor_3d[N][M][P];',
                                  'volatile long int tensor_3d[M][N][P];')
    # Keep every cold global and preserve exact recommended offsets for the hot ones.
    for line in re.findall(r'^volatile long int g_var_.*?;', changed_prefix, re.M):
        changed_prefix = replace_once(changed_prefix, line, '')
    offsets = layouts['globals']['rewritten_offsets']
    if len(offsets) != 16 or len(set(offsets)) != 16 or any(v < 0 or v % 8 for v in offsets):
        raise ValueError('invalid global offsets')
    packed = ['struct alignas(64) PackedGlobals {']
    position = 0
    for index in sorted(range(16), key=offsets.__getitem__):
        offset = offsets[index]
        if offset > position:
            packed.append(f'    unsigned char gap_{position}[{offset-position}];')
        packed.append(f'    volatile long g_var_{index};')
        position = offset + 8
    packed += ['};', 'PackedGlobals packed_globals;']
    packed += [f'static_assert(offsetof(PackedGlobals, g_var_{i}) == {offsets[i]});' for i in range(16)]
    packed += ['volatile long ' + ', '.join(f'g_var_{i}' for i in range(16, 30)) + ';\n']
    changed_prefix += '\n' + '\n'.join(packed)

    def transform(text):
        for obj, layout, field in (('entity_arr', 'entity_layout', 'health'),
                                   ('weapon_arr', 'weapon_layout', 'base_damage')):
            # All access sites in worker, snapshot and address reporting are rewritten.
            text = re.sub(rf'{obj}\[([^\]]+)\]\[([^\]]+)\]\.{field}\b',
                          lambda m: f'{layout}.{field}[{m[2]}][{m[1]}]', text)
            text = re.sub(rf'{obj}\[([^\]]+)\]\[([^\]]+)\]',
                          lambda m: f'{layout}.residual[{m[2]}][{m[1]}]', text)
        for obj in ('matrix_2d', 'tensor_3d'):
            text = re.sub(rf'{obj}\[([^\]]+)\]\[([^\]]+)\]',
                          lambda m: f'{obj}[{m[2]}][{m[1]}]', text)
        text = re.sub(r'\bg_var_(\d+)\b',
                      lambda m: f'packed_globals.{m[0]}' if int(m[1]) < 16 else m[0], text)
        # Address labels are logical names, not C++ access expressions.
        return re.sub(r'"packed_globals\.(g_var_\d+)"', r'"\1"', text)

    rewritten = changed_prefix + transform(body) + main
    output.mkdir(parents=True, exist_ok=True)
    for name, text in (('test_fields2.original.cpp', source), ('test_fields2.baseline.cpp', baseline),
                       ('test_fields2.rewritten.cpp', rewritten)):
        # Original snapshot remains byte-for-byte; generated files drop inherited trailing spaces.
        (output / name).write_text(text if name.endswith('original.cpp') else '\n'.join(line.rstrip() for line in text.splitlines()) + '\n')
    (output / 'test_fields2_support.hpp').write_bytes((HERE / 'test_fields2/test_fields2_support.hpp').read_bytes())
    for old_name, new_name, target in (
            ('test_fields2.original.cpp', 'test_fields2.baseline.cpp', 'shared-safety-and-timing.diff'),
            ('test_fields2.baseline.cpp', 'test_fields2.rewritten.cpp', 'layout-only.diff')):
        diff = difflib.unified_diff((output/old_name).read_text().splitlines(True),
                                    (output/new_name).read_text().splitlines(True), fromfile=old_name, tofile=new_name)
        (output/target).write_text(''.join(diff))
    return {'original_source_sha256': hashlib.sha256(source.encode()).hexdigest(),
            'schema': schema, 'layouts': layouts,
            'scope': 'complete test_fields2 A-D worker; identical atomic_ref config repair in both variants; health remains scheduling-dependent'}


def verify_state(record, metadata):
    n = record['iterations']
    even, fifth = (n + 1) // 2, (n + 4) // 5
    actual = iter(record['state'])
    deterministic = []
    health = []
    config = []

    def expect(expected, location):
        value = next(actual)
        if value != expected:
            raise ValueError(f'{location}: expected {expected}, got {value}')
        deterministic.append(value)

    entity = {'mana': -n, 'velocity_x': even, 'velocity_y': even,
              'score': 10*fifth, 'level': fifth, 'experience': 5*fifth, 'gold': fifth}
    weapon = {'base_damage': 5*n, 'durability': -n, 'critical_chance': even,
              'attack_speed': 2*even, 'element_type': 1, 'weight': fifth}
    for i in range(16):
        for t in range(8):
            for typename, values in (('ComplexEntity', entity), ('WeaponStats', weapon)):
                for field, count in metadata['schema'][typename]:
                    for k in range(count):
                        location = f'{typename}[{i}][{t}].{field}[{k}]'
                        if typename == 'ComplexEntity' and field == 'health' and t < 4:
                            value = next(actual)
                            if not n*(n+1)//2 <= value <= 4*n*n:
                                raise ValueError(f'{location}: health outside legal scheduling bounds')
                            health.append(value)
                            continue
                        expected = values.get(field, 0) if t < 4 else 0
                        if typename == 'ComplexEntity' and t < 4:
                            if field == 'pos_x': expected = t*even
                            if field == 'pos_y': expected = t*even*(even+1)//2
                        expect(expected, location)
            expect(n if t < 4 else 0, 'matrix')
            for k in range(4): expect(n if t < 4 else 0, 'tensor')
    config_values = {'difficulty': 4*n, 'max_retries': 4*n, 'max_players': 6*even,
                     'timeout_ms': 40*even, 'spawn_rate': 4*fifth, 'drop_rate': 4*fifth}
    for field, count in metadata['schema']['GameConfig']:
        for k in range(count):
            expect(config_values.get(field, 0), f'global_cfg.{field}[{k}]')
        if field in config_values: config.append(config_values[field])
    for g in range(30): expect(n if g < 16 else 0, f'g_var_{g}')
    if next(actual, None) is not None:
        raise ValueError('unexpected extra state')
    return {'deterministic_sha256': hashlib.sha256(json.dumps(deterministic).encode()).hexdigest(),
            'health_sha256': hashlib.sha256(json.dumps(health).encode()).hexdigest(),
            'health_min': min(health), 'health_max': max(health),
            'health_lower_bound': n*(n+1)//2, 'health_upper_bound': 4*n*n,
            'config': config, 'state_values_checked': len(deterministic) + len(health)}


def verify_addresses(record, variant, metadata):
    a = record['addresses']
    for stem, old, new in (('matrix', (64, 8), (8, 128)),
                           ('tensor', (256, 32), (32, 512)),
                           ('entity_health', (2560, 320), (8, 128)),
                           ('entity_mana', (2560, 320), (320, 5120)),
                           ('weapon_damage', (1920, 240), (8, 128))):
        strides = new if variant == 'rewritten' else old
        if (a[f'{stem}_1_0']-a[f'{stem}_0_0'], a[f'{stem}_0_1']-a[f'{stem}_0_0']) != strides:
            raise ValueError(f'incorrect physical strides for {variant} {stem}')
    if variant == 'rewritten':
        if a['entity_health_0_0'] % 64 or a['weapon_damage_0_0'] % 64 or a['g_var_0'] % 64:
            raise ValueError('rewritten regions not cacheline aligned')
        offsets = metadata['layouts']['globals']['rewritten_offsets']
        if [a[f'g_var_{g}']-a['g_var_0']+offsets[0] for g in range(16)] != offsets:
            raise ValueError('incorrect global placement')
