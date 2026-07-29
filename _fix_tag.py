"""Add #define TAG "CuckooCtrl" to all 5 parts if missing."""
import os, re

parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\_parts'
tag_define = '#define TAG "CuckooCtrl"\n'

for fn in os.listdir(parts_dir):
    if not fn.startswith('p') or not fn.endswith('.cc'):
        continue
    fpath = os.path.join(parts_dir, fn)
    with open(fpath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    if '#define TAG' in content:
        print(f'{fn}: TAG already present, skipping')
        continue
    
    # Find the last #include/#define/#pragma line to insert after
    lines = content.split('\n')
    insert_at = len(lines)  # default: end
    
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('#include') or stripped.startswith('#define') or stripped.startswith('#pragma') or stripped.startswith('#ifndef') or stripped.startswith('#endif'):
            insert_at = i + 1
    
    # Also check for existing separator comments
    for i in range(insert_at, min(insert_at + 10, len(lines))):
        stripped = lines[i].strip()
        if stripped == '' or stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            insert_at = i + 1
        else:
            break
    
    lines.insert(insert_at, tag_define)
    
    with open(fpath, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    
    print(f'{fn}: added TAG at line {insert_at+1}')

print('Done')
