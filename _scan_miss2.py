import os, re
d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
for fname in ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']:
    path = os.path.join(d, fname)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    for i, line in enumerate(lines, 1):
        s = line.lstrip()
        if not s:
            continue
        # skip proper comments and code
        if s[:2] == '//' or s[:2] == '/*' or s[0] == '*':
            continue
        if s[0] in '\"'\"#{};/\n':
            continue
        # Check if line contains CJK anywhere and starts with non-code
        has_cjk = any(ord(c) >= 0x4e00 and ord(c) <= 0x9fff for c in s)
        if has_cjk:
            indent = line[:len(line)-len(s)]
            print(f'{fname} L{i}: {indent}{s.rstrip()[:80]}')
print('Done')
