# -*- coding: utf-8 -*-
"""正确合并固件 - 保持原始头部"""
import os

SRC = r"C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build"
OUT = os.path.join(SRC, "esp32s3_16M_merger.bin")

# 使用 esptool 合并（这是标准做法）
cmd = (
    f'python -m esptool merge_bin '
    f'--output "{OUT}" '
    f'--flash_mode dio '
    f'--flash_freq 40m '
    f'--flash_size 16MB '
    f'0x1000 "{SRC}\\bootloader\\bootloader.bin" '
    f'0x8000 "{SRC}\\partition_table\\partition-table.bin" '
    f'0xd000 "{SRC}\\ota_data_initial.bin" '
    f'0x10000 "{SRC}\\xiaozhi.bin" '
    f'0x300000 "{SRC}\\generated_assets.bin"'
)

print("正在合并固件...")
print(cmd)
os.system(cmd)

if os.path.exists(OUT):
    sz = os.path.getsize(OUT)
    print(f"\n合并完成!")
    print(f"输出文件: {OUT}")
    print(f"文件大小: {sz} bytes ({sz/1024/1024:.2f} MB)")
    with open(OUT, 'rb') as f:
        hdr = f.read(8)
    print(f"头部: {':'.join(f'{b:02x}' for b in hdr)}")
    chip_id = hdr[2]
    chips = {0:'ESP32', 1:'ESP32S2', 2:'ESP32S3'}
    print(f"芯片: {chips.get(chip_id, 'UNKNOWN')}")
    print("\n用烧录工具，地址填0x0000，选这个文件烧录即可")
else:
    print("合并失败！")
