# -*- coding: utf-8 -*-
"""合并固件为单bin文件，兼容原厂烧录方式"""
import os, struct

SRC = r"C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build"
OUT = os.path.join(SRC, "esp32s3_16M_merger.bin")

# 按照 flash_args 的地址和文件
segments = [
    (0x1000,   "bootloader/bootloader.bin"),
    (0x8000,   "partition_table/partition-table.bin"),
    (0x10000,  "xiaozhi.bin"),
    (0xd000,   "ota_data_initial.bin"),
    (0x300000, "generated_assets.bin"),
]

# 计算总大小
total_size = segments[-1][0]  # 从最后一个地址开始
for addr, fname in segments:
    fpath = os.path.join(SRC, fname)
    if os.path.exists(fpath):
        total_size = max(total_size, addr + os.path.getsize(fpath))
    else:
        print(f"[WARN] 文件不存在: {fpath}")

# 创建填充好0xFF的缓冲区（Flash擦除状态）
data = bytearray(b'\xff' * total_size)

print(f"合并固件总大小: {total_size} bytes ({total_size/1024/1024:.2f} MB)")
print(f"固件将烧录到地址 0x0")

for addr, fname in segments:
    fpath = os.path.join(SRC, fname)
    if not os.path.exists(fpath):
        print(f"[SKIP] 找不到: {fname}")
        continue
    with open(fpath, 'rb') as f:
        bin_data = f.read()
    data[addr:addr+len(bin_data)] = bin_data
    print(f"  [OK] 0x{addr:06x} <- {fname} ({len(bin_data)} bytes)")

with open(OUT, 'wb') as f:
    f.write(data)

# 检查前16字节
with open(OUT, 'rb') as f:
    hdr = f.read(16)

chip_id = hdr[2]
chips = {0:'ESP32', 1:'ESP32S2', 2:'ESP32S3', 3:'ESP32C3'}
print(f"\n芯片识别: {chips.get(chip_id, 'UNKNOWN')} (ID={chip_id})")
print(f"文件大小: {os.path.getsize(OUT)} bytes")
print(f"输出文件: {OUT}")
print("\n用烧录工具，地址填0x0000，选这个文件烧录即可")
