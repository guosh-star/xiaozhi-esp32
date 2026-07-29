"""Fix P3-P5 garbled comments using byte-level matching to avoid Python encoding issues."""
import os, re

PARTS_DIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
PARTS = ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']

# Read replacements from a separate file (avoids Python encoding issues)
REPL_FILE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_map.txt'

replacements = []
with open(REPL_FILE, 'r', encoding='utf-8') as f:
    for line in f:
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        # Format: OLD\tNEW
        parts = line.split('\t', 1)
        if len(parts) == 2:
            old, new = parts[0], parts[1]
            replacements.append((old, new))

# Sort by length descending
replacements.sort(key=lambda x: len(x[0]), reverse=True)

for part_name in PARTS:
    path = os.path.join(PARTS_DIR, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    changed = 0
    for old, new in replacements:
        if old in content:
            content = content.replace(old, new)
            changed += 1
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part_name}: {changed} replacements')
    else:
        print(f'{part_name}: no changes')

print(f'Done. Total: {len(replacements)} mappings.')
