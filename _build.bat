@echo off
chcp 65001 >nul
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32

set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "IDF_TOOLS_PATH=C:\Espressif"

set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PATH%\tools;%IDF_PATH%\tools\esp_tinyuf2;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\esp32s3ulp-elf\2.28.51_20210512\esp32s3ulp-elf\bin;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20240821\openocd-esp32\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.10.2;%PATH%"

echo === Environment ===
echo IDF_PATH=%IDF_PATH%
echo IDF_PYTHON_ENV_PATH=%IDF_PYTHON_ENV_PATH%
echo Python: 
%IDF_PYTHON_ENV_PATH%\Scripts\python.exe --version
echo cmake:
cmake --version 2>&1 | findstr cmake
echo ninja:
ninja --version 2>&1

echo === Running idf.py build ===
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
%IDF_PYTHON_ENV_PATH%\Scripts\python.exe %IDF_PATH%\tools\idf.py build

echo === Build exit code: %errorlevel% ===
if %errorlevel% equ 0 (
    echo SUCCESS!
    dir /b build\*.bin
) else (
    echo BUILD FAILED!
)
