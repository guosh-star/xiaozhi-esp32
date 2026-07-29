import os, sys
d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
total_fixed = 0
skip_start = set(['"', "'", '#', '{', '}', ';', '/', '\n'])
for fname in ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']:
    path = os.path.join(d, fname)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    fixed = 0
    for i in range(len(lines)):
        s = lines[i].lstrip()
        if not s:
            continue
        if s[:2] == '//' or s[:2] == '/*' or s[0] == '*':
            continue
        if s[0] in skip_start:
            continue
        has_cjk = any(ord(c) >= 0x4e00 and ord(c) <= 0x9fff for c in s)
        if has_cjk:
            indent = lines[i][:len(lines[i])-len(s)]
            lines[i] = indent + '// ' + s
            fixed += 1
    if fixed:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(lines)
        print(f'{fname}: fixed {fixed} lines')
        total_fixed += fixed
    else:
        print(f'{fname}: clean')
print(f'Total fixed: {total_fixed}')
