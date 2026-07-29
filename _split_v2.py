"""Split cuckoo_controller.cc into 5 parts by function module.
Each part gets the file's own header block. -c compile only, no link."""
import os, subprocess, sys

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
DST = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
SRC = os.path.join(DST, 'cuckoo_controller.cc')

with open(SRC, 'r', encoding='utf-8-sig') as f:
    lines = f.readlines()
print(f'Source: {len(lines)} lines')

# Find the exact include block end (first line that is NOT a comment/include/define/blank)
header_end = 0
for i, line in enumerate(lines):
    s = line.strip()
    if s.startswith('#include'):
        header_end = i + 1
    elif s.startswith('//') or s == '':
        continue
    elif s.startswith('#define') or s.startswith('#ifndef') or s.startswith('#ifdef'):
        continue
    elif s.startswith('#endif'):
        continue
    else:
        break

HEADER = ''.join(lines[:header_end])
print(f'Header: {header_end} lines')

# Split boundaries (from verified working _parts/ split)
# These are at complete function boundaries
splits = {
    'cuckoo_part1.cc': [0, 1072],       # L1-L1072: Includes, Motor, Servo, Mp3Player
    'cuckoo_part2.cc': [1072, 2047],    # L1073-L2047: LdrSensor, BellSoundPlayer, StateMachine ctor
    'cuckoo_part3.cc': [2047, 2966],    # L2048-L2966: Performance, Alarms, CheckTime
    'cuckoo_part4.cc': [2966, 3911],    # L2967-L3911: DogShow, Linda, Garden, Dance, ShowTask
    'cuckoo_part5.cc': [3911, len(lines)], # L3912-end: MusicDanceTick, Kids, MCP, cuckoo_clock_task
}

for part_name, (start, end) in sorted(splits.items(), key=lambda x: x[0]):
    # Part 1 already has the header, others need it added
    if part_name == 'cuckoo_part1.cc':
        content = ''.join(lines[start:end])
    else:
        content = HEADER + ''.join(lines[start:end])
    
    path = os.path.join(DST, part_name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f'{part_name}: L{start+1}-L{end}, {end-start} lines')

print('\nDone. Run _test_parts.py for -c compilation check.')
