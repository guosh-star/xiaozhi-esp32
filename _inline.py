#!/usr/bin/env python3
"""Clean inline garbled comments on CODE lines - never touches code."""
CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'

with open(CC, 'r', encoding='utf-8') as f:
    L = f.read().splitlines()

# Replacements for known garbled patterns
REPL = {
    'idle': ('idle', '等待状态回到 idle'),
    'call_count_': ('call_count_', '报时次数'),
    'total_calls_': ('total_calls_', '总报时次数'),
    'BirdJumpPulse': ('BirdJumpPulse', '小鸟弹跳脉冲'),
    '1800': ('1800', '等叫声结束 ~1.76s'),
    'CloseBirdDoor': ('CloseBirdDoor', '鸟门关闭'),
    'OpenBirdDoor': ('OpenBirdDoor', '开门 m4_鸟门电机'),
    'hour': ('hour', '24h转12h'),
    '200': ('200', '等状态回到 idle'),
    '3': ('3', '报时3声结束'),
}

for i, l in enumerate(L):
    if chr(0xFFFD) not in l:
        continue
    before_cmt, sep, after_cmt = l.partition('//')
    if not before_cmt.strip():
        continue  # Pure comment line, already handled
    
    # Find the garbled portion and try to replace with known text
    garbled_part = after_cmt.strip()
    found = False
    for key, (code_hint, replacement) in REPL.items():
        if key in before_cmt:
            L[i] = before_cmt + '// ' + replacement
            found = True
            break
    
    if not found:
        # Generic: remove garbled chars, keep only ASCII portion of comment
        clean_after = ''
        for c in after_cmt:
            if ord(c) < 128 or c == ' ':
                clean_after += c
        if clean_after.strip():
            L[i] = before_cmt + '//' + clean_after
        else:
            L[i] = before_cmt.rstrip()

with open(CC, 'w', encoding='utf-8') as f:
    f.write('\n'.join(L))

garb = sum(1 for l in L if chr(0xFFFD) in l)
empty = sum(1 for l in L if l.strip() in ['//', '/**', '*', ' * '])
eng = sum(1 for l in L if l.strip().startswith('//') and len(l.strip()) > 3
          and not any(ord(c) > 127 for c in l.strip())
          and not l.strip().startswith('// ' + '='))

print("Inline garbled cleaned!")
print("Remaining garbled:", garb)
print("Empty markers:", empty)
print("English comments:", eng)
print("Lines:", len(L))
os.remove(__file__)
