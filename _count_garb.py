import os

parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
for p in ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(parts_dir, p)
    count = 0
    garbled_count = 0
    with open(path, 'r', encoding='utf-8') as f:
        for i, line in enumerate(f, 1):
            if '//' in line:
                comment = line.split('//', 1)[1].strip()
                non_ascii = sum(1 for c in comment if ord(c) > 127)
                if non_ascii > 0:
                    has_non_chinese = False
                    for c in comment:
                        if ord(c) <= 127:
                            continue
                        if (0x4e00 <= ord(c) <= 0x9fff or
                            0x3400 <= ord(c) <= 0x4dbf or
                            ord(c) in range(0x3000,0x3030) or
                            ord(c) in range(0xff00,0xffef) or
                            c in '，。！？；：''""（）〔〕【】《》—…·～'):
                            continue
                        has_non_chinese = True
                        break
                    if has_non_chinese:
                        garbled_count += 1
            count += 1
    print(f'{p}: {garbled_count} garbled / {count} total lines')
