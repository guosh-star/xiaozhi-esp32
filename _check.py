#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verify current state and fix all remaining issues."""
import os

CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
BAK = CC.replace('cuckoo_controller.cc', 'backups/2026-07-28/cuckoo_controller.cc.bak_before_annotation_fix')

with open(CC, 'r', encoding='utf-8') as f:
    cur = f.read().splitlines()
with open(BAK, 'r', encoding='utf-8-sig') as f:
    bak = f.read().splitlines()

# Compare code lines (strip inline comments to compare only actual code)
def code_part(line):
    s = line.strip()
    if not s or s.startswith('//') or s.startswith('/*') or s.startswith('*'):
        return None
    idx = s.find('//')
    if idx > 0:
        return s[:idx].rstrip()
    return s

bc = [code_part(l) for l in bak if code_part(l) is not None]
cc = [code_part(l) for l in cur if code_part(l) is not None]

# Check current state
empty = sum(1 for l in cur if l.strip() in ['//', '/**', '*', ' * '])
eng = sum(1 for l in cur if l.strip().startswith('//') and len(l.strip()) > 3
          and not any(ord(c) > 127 for c in l.strip())
          and not l.strip().startswith('// ' + '='))
garb = sum(1 for l in cur if chr(0xFFFD) in l)

print("=== CURRENT STATE ===")
print("Lines:", len(cur))
print("Empty markers:", empty)
print("English comments:", eng)
print("Garbled chars:", garb)
print("Code lines:", len(cc))
print("Code match:", "OK" if bc == cc else "MISMATCH")

if bc != cc:
    diffs = 0
    for i, (a, b) in enumerate(zip(bc, cc)):
        if a != b and diffs < 5:
            print("  DIFF:", a[:60], "vs", b[:60])
            diffs += 1

# Show what empty markers are
print("\nEmpty markers:")
cnt = 0
for i, l in enumerate(cur):
    if l.strip() in ['//', '/**', '*', ' * ']:
        ctx = ""
        for j in range(max(0, i-1), min(len(cur), i+2)):
            cs = cur[j].strip()
            if cs:
                ctx += "[" + cs[:40] + "] "
        print("  L" + str(i+1) + ": \"" + l.strip() + "\" " + ctx.strip())
        cnt += 1
        if cnt >= 20:
            print("  ... and " + str(empty - 20) + " more")
            break

os.remove(__file__)
