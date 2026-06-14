#!/usr/bin/env python3
"""Fix IDF environment and rebuild"""
import os, sys, subprocess, shutil

WORKDIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
IDF_PATH = r'C:\Espressif\frameworks\esp-idf-v5.5.4'
IDF_TOOLS = r'C:\Espressif'
PYTHON_ENV = r'C:\Espressif\python_env\idf5.5_py3.11_env'
CMAKE_BIN = r'C:\Espressif\tools\cmake\3.30.2\bin\cmake.exe'
NINJA_BIN = r'C:\Espressif\tools\ninja\1.12.1\ninja.exe'
GCC_BIN = r'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'

def run(cmd, cwd=None, timeout=120):
    print(f'Running: {cmd[:120]}...')
    env = os.environ.copy()
    env['IDF_PATH'] = IDF_PATH
    env['IDF_TOOLS_PATH'] = IDF_TOOLS
    env['PATH'] = f'{CMAKE_BIN};{GCC_BIN};{os.path.dirname(NINJA_BIN)};{env["PATH"]}'
    
    result = subprocess.run(
        cmd, cwd=cwd or WORKDIR, env=env,
        capture_output=True, text=True, timeout=timeout
    )
    
    # Only print last 50 lines
    out_lines = result.stdout.split('\n')
    err_lines = result.stderr.split('\n')
    if len(out_lines) > 60:
        print('... (truncated)')
        for l in out_lines[-60:]:
            if l.strip():
                print(l)
    else:
        for l in out_lines:
            if l.strip():
                print(l)
    
    if result.stderr:
        for l in err_lines[-20:]:
            if l.strip():
                print('ERR:', l)
    
    print(f'Return code: {result.returncode}')
    return result.returncode == 0

# Step 1: Clean build dir
print('=== Step 1: Full clean ===')
build_dir = os.path.join(WORKDIR, 'build')
if os.path.exists(build_dir):
    shutil.rmtree(build_dir)
    print('Build directory removed')

# Step 2: Run cmake configure via idf.py with explicit IDF_PATH
print('\n=== Step 2: cmake configure ===')
success = run([
    PYTHON_ENV + r'\Scripts\python.exe',
    os.path.join(IDF_PATH, 'tools', 'idf.py'),
    'set-target', 'esp32s3'
])

if not success:
    print('\nTrying direct cmake approach...')
    # Create cmake build dir
    os.makedirs(os.path.join(WORKDIR, 'build'), exist_ok=True)
    success = run([
        CMAKE_BIN, '-B', os.path.join(WORKDIR, 'build'),
        '-G', 'Ninja',
        '-DCMAKE_TOOLCHAIN_FILE=' + os.path.join(IDF_PATH, 'tools', 'cmake', 'toolchain-esp32s3.cmake'),
        '-DIDF_TARGET=esp32s3',
        '-DSDKCONFIG=' + os.path.join(WORKDIR, 'sdkconfig'),
        '-DIDF_PATH=' + IDF_PATH,
        '-DPYTHON=' + os.path.join(PYTHON_ENV, 'Scripts', 'python.exe'),
    ])

print('\n=== Step 3: Build ===')
if not success:
    print('Configuration failed, cannot build')
else:
    run([NINJA_BIN], cwd=os.path.join(WORKDIR, 'build'))

print('\nDone!')
