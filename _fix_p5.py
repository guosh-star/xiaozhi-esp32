"""Add missing declarations to P5 for independent compilation."""
import os

parts_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\_parts'
p5_path = os.path.join(parts_dir, 'p5_music_mcp.cc')

with open(p5_path, 'r', encoding='utf-8') as f:
    content = f.read()

# Declarations to add (after TAG define, before rest of code)
decls = '''// Forward declarations for types/functions defined in P1-P2
struct DoorOpenCtx {
    class CuckooStateMachine* sm;
};
static void ConvertToPcmUrl(char* url, size_t url_sz);

'''

# Insert after TAG define
if '#define TAG' in content:
    idx = content.index('#define TAG')
    # Find end of that line
    end = content.index('\n', idx) + 1
    content = content[:end] + '\n' + decls + content[end:]
    print('Added declarations after TAG define')
else:
    content = decls + content
    print('Prepended declarations')

with open(p5_path, 'w', encoding='utf-8') as f:
    f.write(content)

print(f'P5 updated: {len(content.splitlines())} lines')
