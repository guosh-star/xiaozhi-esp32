with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc','r',encoding='utf-8') as f:
    L = f.read().splitlines()
    
for i, l in enumerate(L):
    if chr(0xFFFD) not in l:
        continue
    # Show the line with position markers
    print(f"Line {i+1}:")
    print(f"  Content: {l[:120]}")
    # Show where the garbled chars are
    pos = [j for j, c in enumerate(l) if c == chr(0xFFFD)]
    print(f"  Garbled positions: {pos}")
    print(f"  Context before: ...{l[max(0,pos[0]-10):pos[0]]}")
    print(f"  Context after: {l[pos[-1]+1:pos[-1]+11]}...")
    print()
