import os
d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
for fname in ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']:
    path = os.path.join(d, fname)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    for i, line in enumerate(lines, 1):
        s = line.lstrip()
        if not s:
            continue
        # skip real comments
        if s.startswith('//') or s.startswith('/*') or s.startswith('*'):
            continue
        # skip code lines
        if s[0] in '"#{}':
            continue
        first = s[0]
        cjk = ord(first) >= 0x4e00 and ord(first) <= 0x9fff
        dash_cjk = first == '-' and len(s) > 1 and ord(s[1]) >= 0x4e00 and ord(s[1]) <= 0x9fff
        if cjk or dash_cjk:
            indent = line[:len(line)-len(s)]
            print(f'{fname} L{i}: {indent}{s.rstrip()[:80]}')
print('Done')
