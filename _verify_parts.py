"""Verify 5 parts vs original cuckoo_controller.cc line-by-line.
Strips duplicate headers from parts 2-5 before comparison."""
import os

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
D = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
ORIG = os.path.join(D, 'cuckoo_controller.cc')
PARTS = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

# Read original
with open(ORIG, 'r', encoding='utf-8') as f:
    orig_lines = f.readlines()
print(f'Original: {len(orig_lines)} lines')

# Read parts
parts_lines = []
header_len = 0
for p in PARTS:
    with open(os.path.join(D, p), 'r', encoding='utf-8') as f:
        lines = f.readlines()
    if p == 'cuckoo_part1.cc':
        header_len = 0  # Part1 has no duplicate header
    else:
        # Find header end in this part (first non-#include, non-comment, non-blank code line)
        h = 0
        for i, l in enumerate(lines):
            s = l.strip()
            if s.startswith('#include'):
                h = i + 1
            elif s.startswith('//') or s == '':
                continue
            elif s.startswith('#define') or s.startswith('#ifndef'):
                continue
            else:
                h = i
                break
        header_len = h
    parts_lines.append(lines[header_len:])
    print(f'{p}: {len(lines)} total, {len(lines[header_len:])} after header strip')

# Concatenate parts content
concat = parts_lines[0]
for pl in parts_lines[1:]:
    concat.extend(pl)
print(f'Concatenated: {len(concat)} lines')

# Compare
mismatches = []
max_lines = max(len(orig_lines), len(concat))
for i in range(max_lines):
    ol = orig_lines[i] if i < len(orig_lines) else '(missing)'
    pl = concat[i] if i < len(concat) else '(missing)'
    if ol.rstrip('\n\r') != pl.rstrip('\n\r'):
        mismatches.append({
            'line': i + 1,
            'orig': ol.rstrip('\n\r')[:120],
            'part': pl.rstrip('\n\r')[:120],
        })

# Report
if not mismatches:
    print('\n✅ 所有 5 个 part 与原始文件完全一致！无遗漏，无错误。')
else:
    print(f'\n❌ {len(mismatches)} 处不一致：')
    for m in mismatches[:50]:
        mid = 'same' if m['orig'] == m['part'] else 'DIFF'
        print(f'  L{m["line"]} {mid}:')
        print(f'    ORIG: {m["orig"]}')
        print(f'    PART: {m["part"]}')
    if len(mismatches) > 50:
        print(f'  ... 还有 {len(mismatches) - 50} 处')

# Line count check
print(f'\n原始: {len(orig_lines)} 行')
print(f'合并: {len(concat)} 行')
if len(orig_lines) == len(concat):
    print('行数一致 ✅')
else:
    print(f'行数差: {len(concat) - len(orig_lines)}')
