"""Add #include headers to parts 2-5 so they can compile independently."""
import os

parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\_parts'

# Extract #include block from p1 (lines up to first non-include/non-define)
with open(os.path.join(parts_dir, 'p1_mp3player.cc'), 'r', encoding='utf-8') as f:
    p1_lines = f.readlines()

include_block = []
for line in p1_lines:
    stripped = line.lstrip()
    if (stripped.startswith('#include') or stripped.startswith('#define') or 
        stripped.startswith('#if') or stripped.startswith('#ifdef') or 
        stripped.startswith('#ifndef') or stripped.startswith('#endif') or
        stripped.startswith('#undef') or stripped.startswith('#pragma') or
        stripped.startswith('#error') or
        stripped == '' or stripped == '\n' or
        stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*') or
        stripped.startswith('extern') or stripped.startswith('static') or
        stripped.startswith('namespace') or stripped.startswith('using') or
        stripped.startswith('TAG =') or stripped.startswith('static const') or
        line.strip() == ''):
        include_block.append(line)
    else:
        break

# Write header block to a file for reference
header_text = ''.join(include_block)
print(f'#include block: {len(include_block)} lines')

# Add to parts 2-5
for part in ['p2_opus_to_pre_cuckoo.cc', 'p3_cuckoo_core.cc', 'p4_shows.cc', 'p5_music_mcp.cc']:
    fpath = os.path.join(parts_dir, part)
    with open(fpath, 'r', encoding='utf-8') as f:
        content = f.readlines()
    
    # Check if already has includes
    if content and content[0].strip().startswith('#'):
        print(f'{part}: already has includes, skipping')
        continue
    
    new_content = include_block + content
    with open(fpath, 'w', encoding='utf-8') as f:
        f.writelines(new_content)
    print(f'{part}: added {len(include_block)} header lines, now {len(new_content)} lines total')

print('Done')
