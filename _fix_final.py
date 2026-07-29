#!/usr/bin/env python
"""Final fix: add unmatched phrases to _garb_map.txt and apply all fixes."""
import os

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
MAP_FILE = os.path.join(BASE, '_garb_map.txt')
PARTS_DIR = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')

# Read existing mappings
with open(MAP_FILE, 'r', encoding='utf-8') as f:
    existing = f.read()

# Add remaining unmapped phrases - use direct string matching from the parts
# Read the actual garbled lines from parts and create exact-match mappings
parts = ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']
garbled_data = {}  # exact_phrase -> correct text

# Known correct replacements for remaining phrases
ADDITIONS = {
    'Mid-music transition: kids became resting → stop & put dog away': 'Mid-music transition: kids became resting → stop & put dog away',
    'Mid-music transition: kids became active → bring dog out': 'Mid-music transition: kids became active → bring dog out',
    'User explicitly asked for kids — force create dog_intro regardless of stale state flags.': 'User explicitly asked for kids — force create dog_intro regardless of stale state flags.',
    # These are actually fine - English text with → character
}

# For each part, find exact garbled comment text and create mapping
for part in parts:
    path = os.path.join(PARTS_DIR, part)
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            if '//' in line:
                comment = line.split('//', 1)[1].strip()
                has_garb = False
                for c in comment:
                    if ord(c) > 127 and not (
                        (0x4e00 <= ord(c) <= 0x9fff) or
                        (0x3400 <= ord(c) <= 0x4dbf) or
                        ord(c) in range(0x3000,0x3030) or
                        ord(c) in range(0xff00,0xffef) or
                        c in ',.!?;:\'\"()[]{}\u2014\u2026\u00b7\uff5e\uff0c\u3002\uff01\uff1f'
                    ):
                        has_garb = True
                        break
                if has_garb:
                    garbled_data[comment] = True

# Now match each actual garbled phrase against our known list
# Use a fuzzy approach: find best match
unmatched_path = os.path.join(BASE, '_garb_unmatched.txt')
with open(unmatched_path, 'r', encoding='utf-8') as f:
    unmatched = [l.strip() for l in f if l.strip()]

# Build a lookup: for each unmatched phrase, find the actual garbled text in parts
# by stripping known-good Chinese chars and comparing "garbled" portions
actual_matches = {}
for actual in garbled_data:
    for un in unmatched:
        # Try exact match first
        if actual == un:
            actual_matches[un] = actual
            break
        # Try: the unmatched phrase is a loose representation
        if un in actual:
            actual_matches[un] = actual
            break

# Now write the complete fixed map
final_map = {}

# First, parse existing tab-separated mappings
for line in existing.splitlines():
    if '\t' in line and not line.startswith('#'):
        old, new = line.split('\t', 1)
        final_map[old] = new

# Add new mappings from actual file content
for actual_text in garbled_data:
    # Try to find best match from final_map keys
    if actual_text in final_map:
        continue
    # Find closest match by common prefix/suffix
    best = None
    best_score = 0
    for key in final_map:
        # Count matching chars at start
        score = 0
        for i in range(min(len(actual_text), len(key))):
            if actual_text[i] == key[i]:
                score += 1
            else:
                break
        if score > best_score and score > 5:
            best_score = score
            best = key
    
    if best and best_score > len(best) * 0.5:
        final_map[actual_text] = final_map[best]

print(f'Final map: {len(final_map)} entries')

# Write the complete map
with open(MAP_FILE, 'w', encoding='utf-8') as f:
    for old, new in sorted(final_map.items(), key=lambda x: -len(x[0])):
        f.write(old + '\t' + new + '\n')

print('Map file updated with fuzzy matches')

# Now apply the fixes
for part_name in ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(PARTS_DIR, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    changed = 0
    for old, new in final_map.items():
        if old in content:
            content = content.replace(old, new)
            changed += 1
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part_name}: {changed} replacements')
    else:
        print(f'{part_name}: no changes')

print('Done!')
