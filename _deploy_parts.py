"""Deploy 5 parts as independent .cc files replacing cuckoo_controller.cc"""
import os, shutil

parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\_parts'
target_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

# Which lines to strip from each part (the header block added for independent compilation)
# P1: needs no stripping (it has the original header)
# P2-P5: strip the #include block (51 lines) + TAG define (1 line)
# Also strip the forward declarations added to P5

parts_config = {
    'p1_mp3player.cc': {'strip': 0, 'source': 'p1_mp3player.cc'},
    'p2_opus_to_pre_cuckoo.cc': {'strip': 52, 'source': 'p2_opus_to_pre_cuckoo.cc'},  # 51 includes + 1 TAG
    'p3_cuckoo_core.cc': {'strip': 52, 'source': 'p3_cuckoo_core.cc'},
    'p4_shows.cc': {'strip': 52, 'source': 'p4_shows.cc'},
    'p5_music_mcp.cc': {'strip': 66, 'source': 'p5_music_mcp.cc'},  # 51 includes + 1 TAG + 14 forward decls
}

# Backup original
original = os.path.join(target_dir, 'cuckoo_controller.cc')
bak = original + '.parts_backup'
if os.path.exists(original):
    shutil.copy(original, bak)
    os.remove(original)
    print(f'Backed up: cuckoo_controller.cc -> cuckoo_controller.cc.parts_backup')
    print(f'Removed: cuckoo_controller.cc')
    print()

for part_name, config in parts_config.items():
    src = os.path.join(parts_dir, config['source'])
    dst = os.path.join(target_dir, part_name)
    
    with open(src, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    # Strip header prefix
    stripped = lines[config['strip']:]
    
    with open(dst, 'w', encoding='utf-8') as f:
        f.writelines(stripped)
    
    print(f'Deployed {part_name}: {len(stripped)} lines (stripped {config["strip"]})')

# Verify: all parts together should match the original backup
print()
print('Verification: assembling stripped parts...')
all_lines = []
for part_name in parts_config:
    with open(os.path.join(target_dir, part_name), 'r', encoding='utf-8') as f:
        all_lines.extend(f.readlines())
bak_lines = open(bak, 'r', encoding='utf-8-sig').readlines()
match = len(all_lines) == len(bak_lines)
if not match:
    print(f'WARNING: assembled {len(all_lines)} lines vs original {len(bak_lines)}')
else:
    # Byte-level comparison of non-comment code
    diffs = 0
    for i in range(len(all_lines)):
        a = all_lines[i].rstrip()
        b = bak_lines[i].rstrip()
        if a != b:
            diffs += 1
            if diffs <= 3:
                print(f'  Diff at L{i+1}:')
                print(f'    NEW: {a[:120]}')
                print(f'    BAK: {b[:120]}')
    print(f'Differences: {diffs} lines out of {len(all_lines)}')
