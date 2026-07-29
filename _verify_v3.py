"""Verify 5 parts vs original cuckoo_controller.cc - exact line match."""
import os, sys
sys.stdout.reconfigure(encoding='utf-8')

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

# Read original
with open(os.path.join(d, 'cuckoo_controller.cc'), encoding='utf-8-sig') as f:
    orig = [l.rstrip('\n\r') for l in f.readlines()]
print(f'Original: {len(orig)} lines')

# Read parts - strip duplicate header (first 49 lines) from P2-P5
parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']
combined = []
p1_header_end = 0

for pi, p in enumerate(parts):
    with open(os.path.join(d, p), encoding='utf-8') as f:
        lines = [l.rstrip('\n\r') for l in f.readlines()]
    
    if pi == 0:  # Part 1: keep everything
        content = lines
    else:  # Strip duplicate header
        # Find where the header (includes/comments/defines) ends
        hdr = 0
        for i, l in enumerate(lines):
            s = l.strip()
            if s.startswith('#include'):
                hdr = i + 1
            elif not s or s.startswith('//'):
                continue
            elif s.startswith('#define') or s.startswith('#ifndef') or s.startswith('#ifdef') or s.startswith('#endif'):
                hdr = i + 1
            else:
                break
        content = lines[hdr:]
    
    combined.extend(content)
    print(f'{p}: {len(lines)} total -> {len(content)} code')

print(f'Combined: {len(combined)} lines')

# Compare
diffs = []
for i in range(max(len(orig), len(combined))):
    ol = orig[i] if i < len(orig) else '<<MISSING>>'
    cl = combined[i] if i < len(combined) else '<<MISSING>>'
    if ol != cl:
        diffs.append((i+1, ol[:120], cl[:120]))

if not diffs:
    print('\n===== PERFECT MATCH =====')
    print('All 5174 lines identical. No errors, no omissions.')
else:
    print(f'\n===== {len(diffs)} DIFFERENCES =====')
    for ln, o, c in diffs[:30]:
        print(f'L{ln}:')
        print(f'  ORG: {o}')
        print(f'  PTC: {c}')
    if len(diffs) > 30:
        print(f'  ... {len(diffs)-30} more')

print(f'\nOriginal lines: {len(orig)}')
print(f'Combined lines: {len(combined)}')
