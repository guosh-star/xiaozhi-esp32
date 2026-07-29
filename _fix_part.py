"""Fix garbled comments in a single part file. Only touches pure comment lines."""
import re, sys

if len(sys.argv) < 2:
    print("Usage: python _fix_part.py <part_file>")
    sys.exit(1)

fpath = sys.argv[1]

with open(fpath, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Garbled unicode replacement characters
garbled = re.compile('[\ufffd\ufffe\uffff]')

fixed = 0
for i, line in enumerate(lines):
    stripped = line.lstrip()
    
    # Skip non-comment lines
    is_pure_comment = (
        stripped.startswith('//') or
        stripped.startswith('/*') or
        stripped.startswith('*') or
        stripped.startswith('/**')
    )
    if not is_pure_comment:
        continue
    
    # Only fix if garbled chars present
    if garbled.search(line):
        newline = garbled.sub('', line)
        lines[i] = newline
        fixed += 1

with open(fpath, 'w', encoding='utf-8') as f:
    f.writelines(lines)

print(f'{fpath}: fixed {fixed} garbled comment lines')
