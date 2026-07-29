#!/usr/bin/env python
"""Fix the absolute last garbled lines - read directly from files."""
import os

PARTS_DIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

# Find and fix remaining garbled lines
for part in ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(PARTS_DIR, part)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    modified = False
    for i, line in enumerate(lines):
        original = line
        # Check for known garbled substrings and replace inline
        # Using string.replace on the raw bytes-like content
        
        # Pattern: listening + garbled + idle -> correct Chinese
        # These are the last few lines that still have garbled unicode
        
        # Try basic character replacements
        new_line = line
        
        # Line-specific fixes
        if 'listening' in line and 'idle' in line and i > 570:
            # Part3 line 582 area
            new_line = line
            # Replace garbled chars between listening and idle
            if '\uf497' in new_line:
                new_line = new_line.replace('\uf497', '')
        
        if new_line != original:
            lines[i] = new_line
            modified = True
    
    if modified:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(lines)
        print(f'{part}: fixed')
    else:
        print(f'{part}: no changes')

print('Done')
