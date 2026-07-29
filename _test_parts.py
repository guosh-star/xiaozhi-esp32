"""Compile each part as a temp file in the original directory. No path hacks needed."""
import subprocess, os, glob, json, shutil

parent_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
build_dir = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build'
parts_dir = os.path.join(parent_dir, '_parts')

with open(os.path.join(build_dir, 'compile_commands.json'), 'r', encoding='utf-8') as f:
    db = json.load(f)

for e in db:
    if 'cuckoo_board.cc' in e.get('command', ''):
        ref_cmd = e['command']
        break

parts = sorted(glob.glob(os.path.join(parts_dir, 'p?_*.cc')))

for part_path in parts:
    part_name = os.path.basename(part_path)
    
    # Copy part to parent dir as temp file (so all includes and output paths work naturally)
    temp_src = os.path.join(parent_dir, '_part_test_' + part_name)
    shutil.copy(part_path, temp_src)
    
    # Replace cuckoo_board.cc with our temp file in the command
    cmd = ref_cmd.replace('cuckoo_board.cc', '_part_test_' + part_name)
    # Change .obj to .o to avoid conflating with ninja
    cmd = cmd.replace('cuckoo_board.cc.obj', '_part_test_' + part_name + '.o')
    
    # Write to .bat to avoid 8191-char limit
    bat = os.path.join(parent_dir, '_run_part.bat')
    with open(bat, 'w', encoding='utf-8') as f:
        f.write(f'@echo off\ncd /d "{build_dir}"\n{cmd}\n')
    
    result = subprocess.run([bat], capture_output=True, text=True, timeout=60)
    
    # Cleanup temps
    try:
        os.remove(temp_src)
        obj = os.path.join(parent_dir, '_part_test_' + part_name + '.o')
        if os.path.exists(obj): os.remove(obj)
        os.remove(bat)
    except:
        pass
    
    if result.returncode == 0:
        print(f'{part_name}: PASS')
    else:
        output = result.stdout + result.stderr
        errors = [l.strip() for l in output.split('\n') if 'error:' in l and 'warning' not in l]
        if errors:
            print(f'{part_name}: FAIL ({len(errors)} errors)')
            for e in errors[:3]:
                msg = e
                if '_part_test_' in msg:
                    idx = msg.index('_part_test_')
                    msg = msg[idx+len('_part_test_'+part_name+':'):]
                elif 'C:' in msg:
                    idx = msg.rfind('C:')
                    tail = msg[idx:]
                    if '_parts/' in tail:
                        msg = tail[tail.index('_parts/'):]
                    else:
                        msg = tail
                print(f'  {msg[:200]}')
        else:
            print(f'{part_name}: FAIL (rc={result.returncode}, no error lines)')
    print()
