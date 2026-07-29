# Clean garbled unicode chars from PURE COMMENT LINES only.
# Never touches lines that contain code (even with trailing comments).
# Pure comment line = after stripping leading spaces, starts with // or * or /**

import re, os

SRC = r'main\boards\cuckoo-clock\cuckoo_controller.cc'
BAK = SRC + '.clean_bak'

# Read with BOM-aware encoding
with open(SRC, 'r', encoding='utf-8-sig') as f:
    lines = f.readlines()

# Backup
with open(BAK, 'w', encoding='utf-8') as f:
    f.writelines(lines)

# Garbled char = U+FFFD (replacement character) or other control chars
garbled = re.compile('[\ufffd\ufffe\uffff]')

cleaned = 0
orphan = 0
for i, line in enumerate(lines):
    stripped = line.lstrip()
    # Determine if this is a pure comment line
    is_pure_comment = (
        stripped.startswith('//') or
        stripped.startswith('/*') or
        stripped.startswith('* ') or
        stripped.startswith('*/') or
        stripped == '*\n' or
        stripped.startswith('/**')
    )
    if not is_pure_comment:
        continue
    
    # Check if line has garbled chars
    if garbled.search(line):
        newline = garbled.sub('', line)  # Remove garbled chars
        # If stripping made the line empty whitespace or minimal, keep as comment marker
        if newline.strip() in ('//', '/*', '*', '*/', '/**', ''):
            lines[i] = newline
        else:
            lines[i] = newline
        cleaned += 1
    # Also handle orphan */ (stray closing without content)
    elif stripped == '*/\n':
        # Check if previous line is empty (orphan scenario)
        prev_empty = (i == 0) or (lines[i-1].strip() == '')
        # Check if next line starts with non-comment code
        next_code = (i+1 < len(lines)) and lines[i+1].strip() and not lines[i+1].lstrip().startswith(('//','/*','*','/**'))
        if prev_empty or next_code:
            lines[i] = '\n'  # Remove orphan */
            orphan += 1

# Write result
with open(SRC, 'w', encoding='utf-8') as f:
    f.writelines(lines)

print(f'Lines processed: {len(lines)}')
print(f'Garbled comment lines cleaned: {cleaned}')
print(f'Orphan */ removed: {orphan}')
print(f'Backup saved to: {BAK}')
