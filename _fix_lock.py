import shutil, os

src = r'D:\xiaozhi备份\xiaozhi-esp32\dependencies.lock'
dst = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\dependencies.lock'
shutil.copy(src, dst)
print(f'Restored correct dependencies.lock ({os.path.getsize(src)} bytes)')

with open(src, 'r', encoding='utf-8') as f:
    content = f.read()
for kw in ['1.7.0', '2.3.1', '1.8.0', '7ff63a7']:
    print(f'  "{kw}" in lock: {kw in content}')

mc = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\managed_components'
if os.path.exists(mc):
    shutil.rmtree(mc)
    print('Deleted managed_components/')
print('Ready for fullclean rebuild')
