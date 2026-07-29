import os, hashlib

backup = r'main\boards\cuckoo-clock\backups\2026-07-28\xiaozhi_2026-07-28_normal.bin'
newbuild = r'build\xiaozhi.bin'

bsz = os.path.getsize(backup)
nsz = os.path.getsize(newbuild)

bh = hashlib.sha256(open(backup, 'rb').read()).hexdigest()
nh = hashlib.sha256(open(newbuild, 'rb').read()).hexdigest()

print(f'Backup firmware: {bsz} bytes, SHA256: {bh}')
print(f'New build:       {nsz} bytes, SHA256: {nh}')
print(f'Size delta:      {nsz-bsz:+d} bytes ({100*(nsz-bsz)/bsz:+.2f}%)')
print(f'SHA256 match:    {bh == nh}')
print()

# Byte-level diff
ba = open(backup, 'rb').read()
nb = open(newbuild, 'rb').read()
minlen = min(len(ba), len(nb))
diff_positions = []
for i in range(minlen):
    if ba[i] != nb[i]:
        diff_positions.append(i)
        if len(diff_positions) > 200:
            break

print(f'First {len(diff_positions)} diff positions in {minlen} bytes:')
for p in diff_positions[:30]:
    ctx_bak = ' '.join(f'{b:02x}' for b in ba[max(0,p-4):p+8])
    ctx_new = ' '.join(f'{b:02x}' for b in nb[max(0,p-4):p+8])
    print(f'  offset 0x{p:08x}: bak={ba[p]:02x} [{ctx_bak}]')
    print(f'               new={nb[p]:02x} [{ctx_new}]')

if len(diff_positions) > 30:
    print(f'  ... and {len(diff_positions)-30} more diff positions')
print(f'Approx different bytes: >={len(diff_positions)} (only first {minlen} bytes scanned)')
