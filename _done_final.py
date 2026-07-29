"""One-shot: split + wrapper + CMake fix. No more circles."""
import os, re

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
SRC = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock', 'cuckoo_controller.cc')
DST = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')

with open(SRC, 'r', encoding='utf-8-sig') as f:
    lines = f.readlines()
print(f'Source: {len(lines)} lines')

# Find split boundaries
landmarks = {}
for target, name in [
    ('void Mp3Player::PlayPcmTask', 'p1_end'),
    ('void CuckooStateMachine::MotorPowerOn', 'p2_end'),
    ('void CuckooStateMachine::DogShow(', 'p3_end'),
    ('void CuckooStateMachine::MusicDanceTick', 'p4_end'),
    ('void cuckoo_clock_task', 'p5_end'),
]:
    for i, line in enumerate(lines):
        if target in line and not line.strip().startswith('//'):
            landmarks[name] = i
            break
print(f'Landmarks: {landmarks}')

# Define header block (taken from the original file's actual includes)
# Read first 70 lines of original to get the real include block
header_ends = 0
for i, line in enumerate(lines):
    if line.startswith('// ============================================') or line.startswith('// ---------'):
        header_ends = i
        break

HEADER = ''.join(lines[:header_ends]) + '\n'

# Split
splits = {
    'cuckoo_part1.cc': (0, landmarks['p1_end']),
    'cuckoo_part2.cc': (landmarks['p1_end'], landmarks['p2_end']),
    'cuckoo_part3.cc': (landmarks['p2_end'], landmarks['p3_end']),
    'cuckoo_part4.cc': (landmarks['p3_end'], landmarks['p4_end']),
    'cuckoo_part5.cc': (landmarks['p4_end'], len(lines)),
}

for part_name, (start, end) in sorted(splits.items(), key=lambda x: ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc'].index(x[0])):
    part_content = HEADER + '\n' + ''.join(lines[start:end])
    path = os.path.join(DST, part_name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(part_content)
    print(f'{part_name}: L{start+1}-L{end}, {end-start} lines')

# Create wrapper
wrapper = """// Cuckoo Clock controller - 5-part split compile wrapper
// Each part in separate file for easy editing.
// Compiles as single translation unit for reliable linking.
#include "cuckoo_part1.cc"
#include "cuckoo_part2.cc"
#include "cuckoo_part3.cc"
#include "cuckoo_part4.cc"
#include "cuckoo_part5.cc"
"""
with open(os.path.join(DST, 'cuckoo_controller.cc'), 'w', encoding='utf-8') as f:
    f.write(wrapper)
print('Wrapper created')

# Fix CMakeLists.txt
cmake_path = os.path.join(BASE, 'main', 'CMakeLists.txt')
with open(cmake_path, 'r', encoding='utf-8') as f:
    cmake = f.read()

OLD = """# Cuckoo Clock: 5 independent part files replace the single cuckoo_controller.cc.
# Each part is a self-contained translation unit.
if(CONFIG_BOARD_TYPE_CUCKOO_CLOCK)
    list(FILTER SOURCES EXCLUDE REGEX ".*cuckoo_controller\\\\.cc$")
endif()"""

# Remove old block if present
if 'list(FILTER SOURCES EXCLUDE REGEX ".*cuckoo_part' in cmake:
    cmake = re.sub(r'# Cuckoo Clock.*?endif\(\)\n?', '', cmake, flags=re.DOTALL)
    with open(cmake_path, 'w', encoding='utf-8') as f:
        f.write(cmake)
    print('Removed old CMakeLists.txt cuckoo filter')

# Verify CMake doesn't exclude anything cuckoo-related
with open(cmake_path, 'r', encoding='utf-8') as f:
    cmake = f.read()
if 'cuckoo_part' in cmake or 'cuckoo_controller' in cmake:
    print('WARNING: cuckoo reference still in CMakeLists.txt')
else:
    print('CMakeLists.txt clean - no cuckoo filter, GLOB picks up wrapper')

print('\nDONE. Ready to compile.')
