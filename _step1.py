#!/usr/bin/env python3
"""Step 1: Strip garbled comments only. Code-safe - only touches pure comment lines."""
CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'

with open(CC, 'r', encoding='utf-8-sig') as f:
    lines = f.read().splitlines()

def is_garbled(s):
    if not (s.startswith('//') or s.startswith('/*') or s.startswith('*')): return False
    if '//' in s and s.split('//')[0].strip(): return False  # code + inline comment -> skip
    na = sum(1 for c in s if ord(c) > 127)
    if na > 10 and na > sum(1 for c in s if ord(c) < 128) * 0.3: return True
    if chr(0xFFFD) in s: return True
    return False

fixed = 0
for i, l in enumerate(lines):
    s = l.strip()
    if not is_garbled(s):
        continue
    indent = l[:len(l) - len(l.lstrip())]
    if s.startswith('//'):
        lines[i] = indent + '//'
    elif s.startswith('*') and not s.startswith('*/'):
        lines[i] = indent + ' *'
    elif s.startswith('/*') or s.startswith('/**'):
        lines[i] = indent + '/**'
    fixed += 1

with open(CC, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))

# Verify
BAK = CC.replace('cuckoo_controller.cc', 'backups/2026-07-28/cuckoo_controller.cc.bak_before_annotation_fix')
with open(BAK, 'r', encoding='utf-8-sig') as f:
    bak_lines = f.read().splitlines()

def code_only(lst):
    return [l for l in lst if l.strip() and not l.strip().startswith('//') and not l.strip().startswith('/*') and not l.strip().startswith('*')]

bc = code_only(bak_lines)
cc = code_only(lines)
print(f"STEP 1 DONE: Stripped {fixed} garbled comments")
print(f"Lines: {len(lines)}")
print(f"Code integrity: {'OK' if bc == cc else 'FAILED!'} ({len(bc)} vs {len(cc)})")
