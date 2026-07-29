#!/usr/bin/env python
"""Count remaining garbled lines in P3-P5."""
import os

parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

garbled_lines = []
for p in ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(parts_dir, p)
    with open(path, 'r', encoding='utf-8') as f:
        for i, line in enumerate(f, 1):
            if '//' in line:
                comment = line.split('//', 1)[1].strip()
                has = False
                for c in comment:
                    if ord(c) <= 127:
                        continue
                    if (0x4e00 <= ord(c) <= 0x9fff or
                        0x3400 <= ord(c) <= 0x4dbf or
                        ord(c) in range(0x3000,0x3030) or
                        ord(c) in range(0xff00,0xffef) or
                        c in ',.!?;:\'\"()[]{}\u2014\u2026\u00b7\uff5e\uff0c\u3002\uff01\uff1f\uff1b\uff1a\u2018\u2019\u201c\u201d\uff08\uff09\u3010\u3011\u300a\u300b'):
                        continue
                    has = True
                    break
                if has:
                    garbled_lines.append((p, i, comment[:120]))

phrases = sorted(set(c for p,i,c in garbled_lines))

with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_remaining.txt', 'w', encoding='utf-8') as f:
    f.write(f'Total garbled lines: {len(garbled_lines)}\n')
    f.write(f'Unique phrases: {len(phrases)}\n\n')
    for ph in phrases:
        f.write(ph + '\n')

with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_result.txt', 'w', encoding='utf-8') as f:
    f.write(f'P3-P5 remaining garbled: {len(garbled_lines)} lines, {len(phrases)} unique\n')

print(f'Done: {len(garbled_lines)} garbled, {len(phrases)} unique')
