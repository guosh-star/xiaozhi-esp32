"""Clean garbled chars from inline comments and code strings in a part file."""
import re, sys

if len(sys.argv) < 2:
    print("Usage: python _fix_inline.py <part_file>")
    sys.exit(1)

fpath = sys.argv[1]

with open(fpath, 'r', encoding='utf-8') as f:
    lines = f.readlines()

garbled = re.compile('[\ufffd\ufffe\uffff]')
fixed = 0

for i, line in enumerate(lines):
    if not garbled.search(line):
        continue
    
    # Case 1: inline comment - clean only the // comment part
    if '//' in line:
        code_part, sep, comment_part = line.partition('//')
        new_comment = garbled.sub('', comment_part)
        if new_comment != comment_part:
            lines[i] = code_part + sep + new_comment
            fixed += 1
    # Case 2: code line with garbled (string literals) - clean everywhere
    else:
        newline = garbled.sub('', line)
        if newline != line:
            lines[i] = newline
            fixed += 1

with open(fpath, 'w', encoding='utf-8') as f:
    f.writelines(lines)

print(f'{fpath}: fixed {fixed} lines')
