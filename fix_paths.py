import re

path = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

# Fix 1: /stream? conversion block no.1 (full URL path)
old1 = '''        char* sp = strstr(conv_url, "/stream?");
        if (sp) {
            memcpy(sp, "/opus?", 6);
            memmove(sp + 6, sp + 8, strlen(sp + 8) + 1);
        }
        return mp3_->PlayOpus(conv_url);'''
new1 = '''        char* sp = strstr(conv_url, "/stream?");
        if (sp) {
            memcpy(sp, "/opus?", 6);
            memmove(sp + 6, sp + 8, strlen(sp + 8) + 1);
        }
        sp = strstr(conv_url, "/pcm?");
        if (sp) {
            memcpy(sp, "/opus?", 6);
            memmove(sp + 6, sp + 8, strlen(sp + 8) + 1);
        }
        return mp3_->PlayOpus(conv_url);'''
if old1 in c:
    c = c.replace(old1, new1)
    print("Fix 1 applied (full URL path)")
else:
    print("Fix 1: not found in file, searching...")
    # Try with different whitespace
    for line in c.split('\n'):
        if 'strstr(conv_url, "/stream?"' in line:
            print(f"  Found at line: {line.strip()}")

# Fix 2: /stream? conversion block no.2 (proxy path)
old2 = '''    char* sp2 = strstr(full_url, "/stream?");
    if (sp2) {
        memcpy(sp2, "/opus?", 6);
        memmove(sp2 + 6, sp2 + 8, strlen(sp2 + 8) + 1);
    }
    return mp3_->PlayOpus(full_url);'''
new2 = '''    char* sp2 = strstr(full_url, "/stream?");
    if (sp2) {
        memcpy(sp2, "/opus?", 6);
        memmove(sp2 + 6, sp2 + 8, strlen(sp2 + 8) + 1);
    }
    sp2 = strstr(full_url, "/pcm?");
    if (sp2) {
        memcpy(sp2, "/opus?", 6);
        memmove(sp2 + 6, sp2 + 8, strlen(sp2 + 8) + 1);
    }
    return mp3_->PlayOpus(full_url);'''
if old2 in c:
    c = c.replace(old2, new2)
    print("Fix 2 applied (proxy path)")
else:
    print("Fix 2: not found")

# Fix 3: Update MCP description
old3 = "'/pcm?q=SONG+SINGER'"
new3 = "'/opus?q=SONG+SINGER'"
if old3 in c:
    c = c.replace(old3, new3)
    print("Fix 3 applied (MCP description)")
else:
    print("Fix 3: not found")

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)

print("Done.")
