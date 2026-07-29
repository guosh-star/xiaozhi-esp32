import os

cc_path = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
with open(cc_path, 'r', encoding='utf-8') as f:
    lines = f.readlines()

out = []
for kw in ['DoorOpenCtx', 'ConvertToPcmUrl']:
    out.append(f'=== {kw} ===')
    found = False
    for i, line in enumerate(lines):
        if kw in line and not line.lstrip().startswith('//') and 'ESP_LOG' not in line:
            start = max(0, i-2)
            end = min(len(lines), i+8)
            for j in range(start, end):
                # Encode as repr to avoid GBK issues
                out.append(f'L{j+1}: {lines[j].rstrip()[:120]}')
            found = True
            break
    if not found:
        out.append(f'NOT FOUND in file')
    out.append('')

with open('_types_report.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(out))
print('Report written to _types_report.txt')
