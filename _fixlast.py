import sys
with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc','r',encoding='utf-8') as f:
    L = f.read().splitlines()
for i,l in enumerate(L):
    if chr(0xFFFD) not in l: continue
    before,_,after = l.partition('//')
    if not before.strip():
        # Pure comment line with garbled
        L[i] = l[:len(l)-len(l.lstrip())] + '//'
    else:
        # Code line with garbled inline comment
        clean = ''.join(c for c in after if ord(c)<128 or c==' ')
        if clean.strip():
            L[i] = before + '// ' + clean.strip()
        else:
            L[i] = before.rstrip()
with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc','w',encoding='utf-8') as f:
    f.write('\n'.join(L))
garb = sum(1 for l in L if chr(0xFFFD) in l)
print('Garbled remaining:', garb)
