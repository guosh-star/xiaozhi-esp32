"""Remove bogus #include from all 5 parts."""
import os
d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
bad = '#include "server/constants.h"\n'
for p in ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']:
    path = os.path.join(d, p)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    if bad in content:
        content = content.replace(bad, '')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{p}: removed')
    else:
        print(f'{p}: clean')
