#!/usr/bin/env python3
"""Fix sdkconfig - must have proper configs that match the Kconfig definitions"""

with open('sdkconfig', 'r', encoding='utf-8') as f:
    content = f.read()
    lines = content.split('\n')

# Fix 1: Add BOARD_TYPE_BREAD_COMPACT_WIFI if missing
has_bread_wifi = any('BOARD_TYPE_BREAD_COMPACT_WIFI' in l for l in lines)
if not has_bread_wifi:
    for i, l in enumerate(lines):
        if 'BOARD_TYPE_BREAD_COMPACT_ESP32' in l:
            lines.insert(i, '')
            lines.insert(i, 'CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y')
            lines.insert(i, '# CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI is not set')
            print(f"Added BOARD_TYPE_BREAD_COMPACT_WIFI=y at line {i}")
            break

# Fix 2: Replace CONFIG_SR_MN_CN_MULTINET7_CN with CONFIG_SR_MN_CN_MULTINET7_QUANT
# (since MULTINET7_CN is not a valid Kconfig name)
fixed = False
for i, l in enumerate(lines):
    if 'SR_MN_CN_MULTINET7_CN=y' in l:
        lines[i] = 'CONFIG_SR_MN_CN_MULTINET7_QUANT=y'
        fixed = True
        print(f"Fixed SR_MN line {i}: MULTINET7_CN -> MULTINET7_QUANT")
        break

if not fixed:
    # Check if MULTINET7_QUANT already exists
    has_quant = any('MULTINET7_QUANT=y' in l for l in lines)
    if not has_quant:
        # Add it
        for i, l in enumerate(lines):
            if 'SR_MN_CN_NONE' in l:
                lines.insert(i+1, 'CONFIG_SR_MN_CN_MULTINET7_QUANT=y')
                print(f"Added MULTINET7_QUANT=y at line {i+1}")
                break

# Fix 3: Remove the invalid CONFIG_SR_MN_CN_MULTINET7_CN line if it still exists
lines = [l for l in lines if 'SR_MN_CN_MULTINET7_CN' not in l or '=y' not in l]

# Write back
with open('sdkconfig', 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))

print("\n=== Verification ===")
with open('sdkconfig', 'r', encoding='utf-8') as f:
    for line in f:
        l = line.strip()
        if any(kw in l for kw in ['BOARD_TYPE_BREAD_COMPACT', 'SR_MN_CN_MULTINET7', 'USE_AUDIO_PROCESSOR']):
            if '=y' in l:
                print(f"  ✅ {l}")
            elif 'not set' in l:
                print(f"  ⚠️  {l}")

print("\nDone!")
