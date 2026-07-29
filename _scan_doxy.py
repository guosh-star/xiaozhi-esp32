"""Find ALL garbled doxygen blocks and English comments in all 5 parts."""
import os, re

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
out_path = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_all_garb.txt'
results = []

for p in ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(d, p)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    # Find doxygen blocks
    in_block = False
    block_lines = []
    block_start = 0
    for i, line in enumerate(lines, 1):
        if '/**' in line and '*/' not in line:
            in_block = True
            block_start = i
            block_lines = [line]
        elif in_block:
            block_lines.append(line)
            if '*/' in line:
                in_block = False
                block = ''.join(block_lines)
                has_garb = False
                for c in block:
                    cp = ord(c)
                    if cp > 127 and not (0x4e00<=cp<=0x9fff or 0x2000<=cp<=0x27bf or 0x3000<=cp<=0x303f or 0xff00<=cp<=0xffef):
                        has_garb = True
                        break
                if has_garb:
                    results.append((p, block_start, 'DOXYGEN', block.rstrip()))
                block_lines = []
        
        # Check English line comments (all English, no Chinese)
        if '//' in line and '/**' not in line and '*/' not in line:
            comment = line.split('//', 1)[1].strip()
            if comment and all(ord(c) <= 127 for c in comment):
                words = comment.split()
                if len(words) >= 3 and comment[0].isupper():
                    results.append((p, i, 'ENGLISH', line.rstrip()))

with open(out_path, 'w', encoding='utf-8') as f:
    for part, ln, typ, text in results:
        f.write(f'[{typ}] {part}:{ln}\n{text}\n\n')

print(f'Total: {len(results)} items (doxygen + english)')
