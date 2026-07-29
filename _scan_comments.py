"""Scan all 5 part files for English and garbled comments."""
import os, re

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

total_eng = 0
total_garb = 0
total_lines = 0
all_eng = []
all_garb = []

# Common valid Unicode symbols that should NOT be flagged as mojibake
VALID_UNICODE_SYMBOLS = {
    0x00b0,  # ° degree sign
    0x00d7,  # × multiplication sign
    0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x2195, 0x2196, 0x2197, 0x2198, 0x2199,  # arrows
    0x21d2, 0x21d4,  # double arrows
    0x25b6, 0x25c0,  # play/pause
    0x2605, 0x2606,  # stars
    0x2714, 0x2716,  # check/cross
    0x3008, 0x3009,  # angle brackets《》
    0x300a, 0x300b,  # double angle brackets
}

def is_garbled(text):
    """Heuristic: detect garbled Chinese (mojibake)"""
    non_ascii = [c for c in text if ord(c) > 127]
    if not non_ascii:
        return False
    # Garbled chars often fall in extended ASCII range (128-255) or
    # have odd codepoint patterns. Real Chinese is in 0x4e00-0x9fff, 0x3400-0x4dbf
    for c in non_ascii:
        cp = ord(c)
        # U+FFFD replacement character is always garbled
        if cp == 0xfffd:
            return True
        # Skip valid Unicode symbols
        if cp in VALID_UNICODE_SYMBOLS:
            continue
        # Skip CJK punctuation / symbols ranges
        if 0x2000 <= cp <= 0x206f: continue
        if 0x3000 <= cp <= 0x303f: continue
        if cp >= 0x4e00: continue  # valid CJK
        if 0x80 <= cp <= 0x3fff:
            return True
    return False

def is_comment_line(line, in_multiline_comment):
    """Check if a line is a comment line (not code starting with *)"""
    stripped = line.strip()
    if stripped == '':
        return False
    if stripped.startswith('//'):
        return True
    if stripped.startswith('/*'):
        return True
    # A line starting with '*' is a comment only if we're inside a /**/ block.
    # Lines like *dst = '\0' are C code (pointer dereference), not comments.
    if stripped.startswith('*') and not stripped.startswith('*/'):
        if in_multiline_comment:
            return True
        rest = stripped[1:].lstrip()
        if not rest:
            return False
        # C code: *identifier followed by operator (=, ++, --, ; etc)
        first_word = re.match(r'[a-zA-Z_][a-zA-Z0-9_]*', rest)
        if first_word:
            after = rest[first_word.end():].lstrip()
            if after and after[0] in '=+-*/%&|<>!?:;':
                return False  # C code assignment/operation
        return True  # Doxygen-style comment
    return False

def is_english_comment(text):
    """Check if a comment line is English (not code reference)"""
    # Strip comment markers
    t = text.strip()
    if t.startswith('//'):
        t = t[2:].strip()
    elif t.startswith('/*'):
        t = t[2:]
    elif t.startswith('*') and not t.startswith('*/'):
        t = t[1:].strip()
    
    if not t:
        return False
    
    # Check if all chars are ASCII
    if not all(ord(c) < 128 for c in t):
        return False
    
    # Must have English words (not just symbols or numbers)
    words = re.findall(r'[a-zA-Z]{3,}', t)
    if not words:
        return False
    
    # Exclude code-like patterns (GPIO, PWM, LED, etc. with underscores)
    code_patterns = r'^(GPIO|PWM|LED|DMA|I2C|SPI|UART|MCPWM|LEDC|RMT|ADC|DAC|I2S|SDIO)_'
    if re.match(code_patterns, t):
        return False
    
    # Exclude lines that are just C code fragments (pointer ops)
    # Like *dst++ = c;  *dst = '\0'; etc
    if re.match(r'^\*[a-z_]+', t):
        return False
    
    return True

for p in parts:
    path = os.path.join(d, p)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    eng_count = 0
    garb_count = 0
    
    in_multiline_comment = False
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        # Track multi-line comment state
        if '/*' in stripped:
            in_multiline_comment = True
        if '*/' in stripped:
            in_multiline_comment = False
        # Check if it's a comment line
        if is_comment_line(line, in_multiline_comment):
            if is_garbled(stripped):
                garb_count += 1
                all_garb.append(f'{p}:{i}: {stripped[:120]}')
            elif is_english_comment(stripped):
                eng_count += 1
                all_eng.append(f'{p}:{i}: {stripped[:120]}')
    
    total_eng += eng_count
    total_garb += garb_count
    total_lines += len(lines)
    print(f'{p}: {eng_count} English, {garb_count} garbled')

print(f'\nTotal: {total_eng} English, {total_garb} garbled across {total_lines} lines')

# Save English comments
with open(os.path.join(d, '..', '..', '..', '_eng_comments.txt'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(all_eng))
print(f'Saved {len(all_eng)} English comments to _eng_comments.txt')

# Save garbled comments  
with open(os.path.join(d, '..', '..', '..', '_garb_comments.txt'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(all_garb))
print(f'Saved {len(all_garb)} garbled comments to _garb_comments.txt')
