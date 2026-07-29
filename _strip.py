"""Strip garbled Chinese from comments, keeping code intact."""
import os

CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'

with open(CC, 'r', encoding='utf-8-sig') as f:
    lines = f.read().splitlines()

cleaned = 0
for i, line in enumerate(lines):
    s = line.strip()
    # Check if it's a comment line with garbled Chinese
    if not (s.startswith('//') or s.startswith('/*') or s.startswith('*') or s.startswith('/**')):
        continue
    na = sum(1 for c in s if ord(c) > 127)
    asc = sum(1 for c in s if ord(c) < 128)
    if na < 10 or na <= asc * 0.3:
        if chr(0xFFFD) not in s:
            continue
    
    # Garbled! Strip the garbled text but keep the comment structure
    indent = line[:len(line) - len(line.lstrip())]
    if s.startswith('//'):
        lines[i] = indent + '// '
        cleaned += 1
    elif s.startswith('*') and not s.startswith('*/'):
        lines[i] = indent + ' * '
        cleaned += 1
    elif s.startswith('/*') or s.startswith('/**'):
        lines[i] = indent + '/**'
        cleaned += 1

with open(CC, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))

print(f'Stripped {cleaned} garbled comments')
print(f'Total lines: {len(lines)}')
print('Done')
