"""Final comprehensive garbled comment fixer.
Reads actual garbled text from files, creates exact-match replacements."""
import os, re

PARTS_DIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
PARTS = ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']

# Allowed non-Chinese non-ASCII chars (these are NOT garbled)
ALLOWED = set('，。！？；：''""（）〔〕【】《》—…·～→←↑↓↔⇒⇐°±×÷≈≠≤≥⌂◆◇○●□■△▲☆★♩♪♫♬、')
# Also allow common Unicode ranges
def is_allowed(c):
    cp = ord(c)
    if cp <= 127: return True
    if 0x4e00 <= cp <= 0x9fff: return True  # CJK Unified
    if 0x3400 <= cp <= 0x4dbf: return True  # CJK Extension A
    if 0x2000 <= cp <= 0x206f: return True  # General Punctuation
    if 0x2100 <= cp <= 0x214f: return True  # Letterlike Symbols
    if 0x2190 <= cp <= 0x21ff: return True  # Arrows
    if 0x2200 <= cp <= 0x22ff: return True  # Math Operators
    if 0x2300 <= cp <= 0x23ff: return True  # Misc Technical
    if 0x2460 <= cp <= 0x24ff: return True  # Enclosed Alphanumerics
    if 0x2500 <= cp <= 0x257f: return True  # Box Drawing
    if 0x25a0 <= cp <= 0x25ff: return True  # Geometric Shapes
    if 0x2600 <= cp <= 0x26ff: return True  # Misc Symbols
    if 0x2700 <= cp <= 0x27bf: return True  # Dingbats
    if 0x3000 <= cp <= 0x303f: return True  # CJK Symbols
    if 0xff00 <= cp <= 0xffef: return True  # Halfwidth/Fullwidth
    if c in ALLOWED: return True
    return False

# Find all genuinely garbled lines
garbled_lines = []
for part in PARTS:
    path = os.path.join(PARTS_DIR, part)
    with open(path, 'r', encoding='utf-8') as f:
        for i, line in enumerate(f, 1):
            if '//' not in line and '/*' not in line:
                continue
            # Extract comment
            comment = ''
            if '//' in line:
                comment = line.split('//', 1)[1].strip()
            # Check for garbled
            for c in comment:
                if not is_allowed(c):
                    garbled_lines.append((part, i, line.rstrip()[:200]))
                    break

print(f'Genuinely garbled lines: {len(garbled_lines)}')

# Extract unique garbled comments
unique_garbled = set()
for part, ln, text in garbled_lines:
    if '//' in text:
        comment = text.split('//', 1)[1].strip()
        unique_garbled.add(comment[:150])

print(f'Unique: {len(unique_garbled)}')

if len(garbled_lines) == 0:
    print('All clean!')
else:
    # Save for inspection
    with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_final.txt', 'w', encoding='utf-8') as f:
        for ph in sorted(unique_garbled, key=lambda x: -len(x)):
            f.write(ph + '\n')
    print('Saved to _garb_final.txt')
    
    # Show first few
    for ph in sorted(unique_garbled, key=lambda x: -len(x))[:10]:
        print(f'  {ph[:100]}')
