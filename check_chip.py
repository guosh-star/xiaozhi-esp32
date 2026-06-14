import struct

f = open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build\xiaozhi.bin', 'rb')
d = f.read(16)
f.close()

print('First 16 bytes:', ':'.join(f'{b:02x}' for b in d))

chip = d[2]
chips = {0:'ESP32', 1:'ESP32S2', 2:'ESP32S3', 3:'ESP32C3', 4:'ESP32C2', 
         5:'ESP32C6', 6:'ESP32H2', 7:'ESP32P4', 9:'ESP32C5', 12:'ESP32H4', 13:'ESP32C61'}
print('Chip ID byte:', chip, '-', chips.get(chip, 'UNKNOWN'))
