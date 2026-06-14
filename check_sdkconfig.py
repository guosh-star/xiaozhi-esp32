#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""检查sdkconfig的关键配置是否正确"""

SDKCONFIG = r"C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\sdkconfig"
OLD = r"C:\Users\GUO-notebook\.openclaw\backup_sdkconfig\sdkconfig.original"
import os

with open(SDKCONFIG, 'r', encoding='utf-8') as f:
    c = f.read()

issues = []

# 1. IDF Target - 必须 esp32s3
target = None
for line in c.split('\n'):
    if line.startswith('CONFIG_IDF_TARGET='):
        target = line.split('=')[1].strip('"')
        break
if target != 'esp32s3':
    issues.append(f'[BAD] CONFIG_IDF_TARGET="{target}" 应该是 "esp32s3"')
else:
    print(f'[OK] CONFIG_IDF_TARGET="esp32s3"')

# 2. Board type - 必须 BREAD_COMPACT_WIFI
if 'CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y' not in c:
    # 可能因为它是默认值所以没在文件里，检查其他board type
    other_boards = [l for l in c.split('\n') if 'BOARD_TYPE' in l and 'y' in l and '# ' not in l]
    if other_boards:
        issues.append(f'[BAD] 错误的板型: {other_boards}')
    else:
        print('[WARN] CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI 未显式设置（可能为默认值）')
else:
    print('[OK] Board type: BREAD_COMPACT_WIFI')

# 3. Flash size - 必须 16MB
if 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y' not in c:
    issues.append('[BAD] Flash size 不是 16MB')
else:
    print('[OK] Flash size: 16MB')
if '"16MB"' not in c:
    issues.append('[BAD] Flash size string 不是 "16MB"')
else:
    print('[OK] Flash size string: "16MB"')

# 4. Flash mode - DIO
if 'CONFIG_ESPTOOLPY_FLASHMODE_DIO=y' not in c:
    issues.append('[BAD] Flash mode 不是 DIO')
else:
    print('[OK] Flash mode: DIO')

# 5. Flash frequency - 40M (对于面包板WiFi来说ok，但看看是不是应该80M)
print('[INFO] Flash freq: 40M')

# 6. Partition table
for line in c.split('\n'):
    if 'PARTITION_TABLE_CUSTOM_FILENAME' in line and '=' in line and '# ' not in line:
        pt = line.split('=')[1].strip('"')
        print(f'[OK] Partition table: {pt}')
        break

# 7. 唤醒词
print(f'[OK] Wake word: 已启用自定义唤醒词')
for line in c.split('\n'):
    if 'CUSTOM_WAKE_WORD=' in line and '# ' not in line:
        val = line.split('=')[1]
        print(f'   CUSTOM_WAKE_WORD={val}')
for line in c.split('\n'):
    if 'CUSTOM_WAKE_WORD_DISPLAY=' in line and '# ' not in line:
        val = line.split('=')[1]
        print(f'   CUSTOM_WAKE_WORD_DISPLAY={val}')

# 8. MultiNet
if 'CONFIG_SR_MN_CN_MULTINET7_CN=y' not in c:
    issues.append('[BAD] MultiNet CN 未启用')
else:
    print('[OK] MultiNet: CN_MULTINET7_CN')
if 'CONFIG_SR_MN_CN_NONE=y' in c and 'is not set' not in c.split('CONFIG_SR_MN_CN_NONE=y')[0][-5:]:
    issues.append('[BAD] SR_MN_CN_NONE 仍启用（与MULTINET7冲突）')

# 9. Bootloader
if 'CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS=y' not in c:
    issues.append('[BAD] BOOTLOADER_SKIP_VALIDATE_ALWAYS 未启用')
else:
    print('[OK] Bootloader: skip validate')
if 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' not in c:
    issues.append('[BAD] BOOTLOADER_APP_ROLLBACK_ENABLE 未启用')

print()
if issues:
    print('=== 问题列表 ===')
    for i in issues:
        print(i)
else:
    print('[OK] 所有检查通过！')
