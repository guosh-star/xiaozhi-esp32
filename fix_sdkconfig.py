#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Fix sdkconfig: board type, flash size, wake word, partition table"""

import sys

SDKCONFIG = r"C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\sdkconfig"

with open(SDKCONFIG, 'r', encoding='utf-8') as f:
    content = f.read()

changes = 0

# 1. Board type: BREAD_COMPACT_ESP32 -> BREAD_COMPACT_WIFI
old = 'CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32=y'
new = '# CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32 is not set'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('1. Disabled old board type')

old = '# CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI is not set'
new = 'CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('2. Enabled BREAD_COMPACT_WIFI board')

# 2. Flash size: 4MB -> 16MB
old = 'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y'
new = '# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('3. Disabled 4MB flash')

old = '# CONFIG_ESPTOOLPY_FLASHSIZE_16MB is not set'
new = 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('4. Enabled 16MB flash')

old = 'CONFIG_ESPTOOLPY_FLASHSIZE="4MB"'
new = 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('5. Flash size string: 4MB -> 16MB')

# 3. Partition table
old = 'partitions/v2/4m.csv'
new = 'partitions/v1/16m_custom_wakeword.csv'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('6. Partition table: 4m.csv -> 16m_custom_wakeword.csv')

# 4. Disable WAKE_WORD_DISABLED
old = 'CONFIG_WAKE_WORD_DISABLED=y'
new = '# CONFIG_WAKE_WORD_DISABLED is not set'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('7. Disabled WAKE_WORD_DISABLED')

# 5. Enable custom wake word
old = '# CONFIG_USE_CUSTOM_WAKE_WORD is not set'
new = 'CONFIG_USE_CUSTOM_WAKE_WORD=y'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('8. Enabled USE_CUSTOM_WAKE_WORD')

# 6. Set custom wake word string
old = 'CONFIG_CUSTOM_WAKE_WORD=""'
new = 'CONFIG_CUSTOM_WAKE_WORD="bu gu niao"'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('9. Set CUSTOM_WAKE_WORD = "bu gu niao"')

old = 'CONFIG_CUSTOM_WAKE_WORD_DISPLAY=""'
new = 'CONFIG_CUSTOM_WAKE_WORD_DISPLAY="布谷鸟"'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('10. Set CUSTOM_WAKE_WORD_DISPLAY = "布谷鸟"')

# 7. MultiNet: enable CN_MULTINET7_CN
old = 'CONFIG_SR_MN_CN_NONE=y'
new = '# CONFIG_SR_MN_CN_NONE is not set'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('11. Disabled SR_MN_CN_NONE')

old = '# CONFIG_SR_MN_CN_MULTINET7_CN is not set'
new = 'CONFIG_SR_MN_CN_MULTINET7_CN=y'
if old in content:
    content = content.replace(old, new)
    changes += 1
    print('12. Enabled SR_MN_CN_MULTINET7_CN')

# 8. Ensure SEND_WAKE_WORD_DATA
if 'CONFIG_SEND_WAKE_WORD_DATA=y' not in content:
    # Find a good place to insert - after custom wake word threshold
    idx = content.find('CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20')
    if idx >= 0:
        nl = content.find('\n', idx)
        if nl >= 0:
            content = content[:nl+1] + 'CONFIG_SEND_WAKE_WORD_DATA=y\n' + content[nl+1:]
            changes += 1
            print('13. Added SEND_WAKE_WORD_DATA=y')

with open(SDKCONFIG, 'w', encoding='utf-8') as f:
    f.write(content)

print(f'\nDone! {changes} changes made to sdkconfig')
