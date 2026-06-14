@echo off
chcp 65001 >nul
echo ========================================
echo Starting ESP-IDF build with direct ninja
echo ========================================

set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "IDF_TOOLS_PATH=C:\Espressif"
set "CCACHE_DISABLE=1"

set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PATH%\tools;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%"

echo Python: 
%IDF_PYTHON_ENV_PATH%\Scripts\python.exe --version
echo cmake:
cmake --version | findstr cmake

echo.
echo === Step 1: Reconfigure cmake (disable ccache) ===
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build
cmake .. -G Ninja -DPYTHON_DEPS_CHECKED=1 -DPYTHON="%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" -DESP_PLATFORM=1 -DCCACHE_ENABLE=0

echo.
echo === Step 2: Build with ninja ===
ninja -j4

echo.
echo === Build exit code: %errorlevel% ===
if %errorlevel% equ 0 (
    echo SUCCESS!
    cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
    dir build\*.bin
) else (
    echo BUILD FAILED
)
pause
