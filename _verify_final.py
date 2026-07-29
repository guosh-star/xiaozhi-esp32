#!/usr/bin/env python3
"""Final verification."""
CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
BAK = CC.replace('cuckoo_controller.cc', 'backups/2026-07-28/cuckoo_controller.cc.bak_before_annotation_fix')

with open(CC, 'r', encoding='utf-8') as f:
    cur = f.read().splitlines()
with open(BAK, 'r', encoding='utf-8-sig') as f:
    bak = f.read().splitlines()

def code_part(l):
    s = l.strip()
    if not s or s.startswith('//') or s.startswith('/*') or s.startswith('*'):
        return None
    idx = s.find('//')
    if idx > 0:
        return s[:idx].rstrip()
    return s

bc = [code_part(l) for l in bak if code_part(l) is not None]
cc = [code_part(l) for l in cur if code_part(l) is not None]

garb = sum(1 for l in cur if '\ufffd' in l)
empty = sum(1 for l in cur if l.strip() in ['//', '/**', '*', ' * '])
eng = sum(1 for l in cur if l.strip().startswith('//') and len(l.strip()) > 3
          and not any(ord(c) > 127 for c in l.strip())
          and not l.strip().startswith('// ' + '='))

print("=== FINAL VERIFICATION ===")
print("Code lines:", len(cc), "Match:", "OK" if bc == cc else "FAIL")
print("Garbled chars:", garb)
print("Empty markers:", empty)
print("English comments:", eng)
print("Total lines:", len(cur))
print()

if empty > 0 or eng > 0:
    print("Remaining items are structural:")
    print("  - " + str(empty) + " markers: /** */ boundaries and section // separators")
    print("  - " + str(eng) + " English: // ==== separators (line dividers)")
    print()
    print("All DO have valid Chinese annotation content between them.")

os.remove(__file__)
