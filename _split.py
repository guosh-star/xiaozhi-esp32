import re

src = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
outdir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\_parts'

import os
os.makedirs(outdir, exist_ok=True)

lines = open(src, 'r', encoding='utf-8-sig').read().splitlines()

# Split points (1-based line numbers, start of major function/class)
parts = [
    (1,       1072,    'p1_mp3player',        'Includes + Motor/Servo/Bell + Mp3Player'),
    (1073,    2047,    'p2_opus_to_pre_cuckoo','PlayOpus/Ogg/Serial up to CuckooStateMachine'),
    (2048,    2966,    'p3_cuckoo_core',       'CuckooStateMachine core: Motor/Door/Dance/Performance/Alarm'),
    (2967,    3911,    'p4_shows',             'DogShow/Linda/Garden/Dance/ShowTask/Music/CloseDoor'),
    (3912,    5174,    'p5_music_mcp',         'MusicDanceTick/Kids/MCP callbacks'),
]

for start, end, name, desc in parts:
    part_lines = lines[start-1:end]
    outpath = os.path.join(outdir, f'{name}.cc')
    with open(outpath, 'w', encoding='utf-8') as f:
        f.write('\n'.join(part_lines) + '\n')
    print(f'{name}.cc:  L{start}-L{end}  ({len(part_lines)} lines)  {desc}')

print(f'\nTotal original: {len(lines)} lines')
total_split = sum(len(lines[s-1:e]) for s, e, _, _ in parts)
print(f'Total split sum: {total_split} lines')
print(f'Match: {total_split == len(lines)}')
