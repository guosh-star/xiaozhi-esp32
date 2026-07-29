"""Fix only comment lines: garbled->Chinese, English->Chinese. Never touch code."""
import os, re, sys

DST = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
FILES = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

# Read all files
all_lines = []
for fname in FILES:
    path = os.path.join(DST, fname)
    with open(path, 'r', encoding='utf-8-sig') as f:
        all_lines.extend(f.readlines())
print(f'Total lines: {len(all_lines)}')

# Now scan the original backup for reference
BACKUP = os.path.join(DST, 'backups', '2026-07-28', 'cuckoo_controller.cc')
with open(BACKUP, 'r', encoding='utf-8-sig') as f:
    backup_lines = f.readlines()

# For each line, check if it's a comment line (starts with //, /*, *)
# and if it contains garbled text (U+FFFD or broken bytes) or English
fixed_count = 0
en_count = 0
garbled_count = 0

for i, line in enumerate(all_lines):
    stripped = line.lstrip()
    # ONLY touch comment lines
    is_comment = stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*')
    if not is_comment:
        continue
    
    # Check for garbled (U+FFFD)
    if '\ufffd' in line:
        # Extract English content and any intact Chinese
        # Use backup line to infer correct text
        if i < len(backup_lines):
            orig = backup_lines[i]
            # The backup also has garbled chars
            # We need to infer from context - function name, nearby code
            garbled_count += 1
            # For now, mark garbled but don't change
            continue
    
    # Check for English comments that should be translated
    # Simple heuristic: comment line with mostly ASCII printable chars
    content = stripped[2:].strip() if stripped.startswith('//') else stripped.strip()
    if content and len(content) > 3:
        # Count non-ASCII
        ascii_chars = sum(1 for c in content if ord(c) < 128)
        total_chars = len(content)
        if total_chars > 0 and ascii_chars / total_chars > 0.85 and any(c.isalpha() for c in content):
            # This is likely an English comment
            en_count += 1

print(f'Garbled comment lines: {garbled_count}')
print(f'English comment lines: {en_count}')
print('Scan done.')
