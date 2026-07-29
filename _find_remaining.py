import os
parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
garbled = set()
for p in ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(parts_dir, p)
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            if '//' not in line:
                continue
            comment = line.split('//', 1)[1].strip()
            for c in comment:
                codepoint = ord(c)
                if codepoint <= 127:
                    continue
                if 0x4e00 <= codepoint <= 0x9fff:
                    continue
                if codepoint in range(0x3000, 0x3030):
                    continue
                if codepoint in range(0xff00, 0xffef):
                    continue
                if c in '，。！？；：''""（）〔〕【】《》—…·～':
                    continue
                garbled.add(comment[:120])
                break

with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_rem2.txt', 'w', encoding='utf-8') as f:
    for g in sorted(garbled, key=lambda x: -len(x)):
        f.write(g + '\n')
print(f'Remaining: {len(garbled)} unique')
