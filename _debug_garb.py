"""Check actual bytes of garbled line to diagnose encoding."""
path = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_part1.cc'
with open(path, 'rb') as f:
    for i, line in enumerate(f, 1):
        if i == 2:
            print(f'Line 2 raw bytes ({len(line)} bytes):')
            print(line[:120].hex(' '))
            print(f'As UTF-8: {line.decode("utf-8", errors="backslashreplace")[:100]}')
            # Try to recover via Latin-1
            decoded_utf8 = line.decode('utf-8', errors='backslashreplace')
            try:
                recovered = decoded_utf8.encode('latin-1').decode('utf-8')
                print(f'Latin-1\xe2\x86\x92UTF-8: {recovered[:100]}')
            except Exception as e:
                # Try CP1252
                try:
                    recovered = decoded_utf8.encode('cp1252').decode('utf-8')
                    print(f'CP1252\xe2\x86\x92UTF-8: {recovered[:100]}')
                except Exception as e2:
                    print(f'Latin-1/CD1252 failed: {e}')
            # Also show individual char codepoints  
            for j, c in enumerate(decoded_utf8[:30]):
                print(f'  [{j}] U+{ord(c):04X} ({c})')
            break
