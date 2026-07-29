import shutil, os, stat, time, glob

build_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build'

# Kill any processes holding files
# First try renaming to break locks
backup_name = build_dir + '.old_' + str(int(time.time()))
try:
    os.rename(build_dir, backup_name)
    print(f'Renamed build to {backup_name}')
except:
    print('Rename failed, trying force delete...')
    # Remove all files first
    for root, dirs, files in os.walk(build_dir, topdown=False):
        for name in files:
            fp = os.path.join(root, name)
            try:
                os.chmod(fp, stat.S_IWRITE)
                os.remove(fp)
            except:
                pass
        for name in dirs:
            dp = os.path.join(root, name)
            try:
                os.rmdir(dp)
            except:
                pass
    try:
        os.rmdir(build_dir)
        print('build directory deleted')
    except Exception as e:
        print(f'Failed to delete build: {e}')

# Also clean up old build directories
for old in glob.glob(build_dir + '.old_*'):
    try:
        shutil.rmtree(old, ignore_errors=True)
        print(f'Cleaned up {old}')
    except:
        pass

print('Done. Ready for rebuild.')
