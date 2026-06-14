@echo off
call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57

cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32

echo ========== Step 1: Clean previous build (keep sdkconfig) ==========
idf.py clean

echo ========== Step 2: Enable wake word via sdkconfig defaults ==========
echo.
echo Configuring: WAKE_WORD -> ESP_WAKE, WN9_NIHAOXIAOZHI_TTS, MN7_QUANT...
echo.
:: We need to set these via menuconfig. Let's do it programmatically:
cd build\config
del sdkconfig.h 2>nul
cd ..\..

:: Directly modify sdkconfig
python -c "
import re

with open('sdkconfig', 'r') as f:
    content = f.read()

# Disable WAKE_WORD_DISABLED
content = content.replace('CONFIG_WAKE_WORD_DISABLED=y', '# CONFIG_WAKE_WORD_DISABLED is not set')

# Enable USE_ESP_WAKE_WORD (add after wake word section)
# First check if it already exists
if 'CONFIG_USE_ESP_WAKE_WORD=y' not in content:
    # Find the WAKE_WORD_DISABLED line and add after its comment block
    content = content.replace(
        '# CONFIG_WAKE_WORD_DISABLED is not set\n',
        '# CONFIG_WAKE_WORD_DISABLED is not set\nCONFIG_USE_ESP_WAKE_WORD=y\n'
    )

# Ensure USE_CUSTOM_WAKE_WORD is NOT set
content = content.replace('CONFIG_USE_CUSTOM_WAKE_WORD=y', '# CONFIG_USE_CUSTOM_WAKE_WORD is not set')
content = content.replace('CONFIG_USE_AFE_WAKE_WORD=y', '# CONFIG_USE_AFE_WAKE_WORD is not set')

with open('sdkconfig', 'w') as f:
    f.write(content)

print('sdkconfig updated successfully')
"

echo.

echo ========== Step 3: Build firmware ==========
idf.py build

echo ========== EXIT CODE: %ERRORLEVEL% ==========

echo.
echo ========== Step 4: Check generated files ==========
dir build\xiaozhi.bin
dir build\bootloader\bootloader.bin
dir build\partition_table\partition-table.bin
dir build\assets.bin 2>nul
dir build\srmodels\srmodels.bin 2>nul

echo.
echo ========== Step 5: Re-generate assets.bin with wake word ==========
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe scripts\build_default_assets.py ^
 --sdkconfig sdkconfig ^
 --builtin_text_font font_puhui_basic_14_1 ^
 --output build\assets.bin

echo.
echo ========== Checking assets.bin size ==========
dir build\assets.bin

echo.
echo ========== ALL DONE ==========
echo.
echo Flash commands for tomorrow:
echo ============================
echo esptool.py --chip esp32s3 -p COM3 -b 921600 --before default_reset --after hard_reset write_flash ^
echo   0x0 build\bootloader\bootloader.bin ^
echo   0x8000 build\partition_table\partition-table.bin ^
echo   0x10000 build\xiaozhi.bin
echo.
echo Then flash model partition (srmodels.bin to model partition):
echo esptool.py --chip esp32s3 -p COM3 -b 921600 write_flash 0x10000 build\srmodels\srmodels.bin
echo.
echo And assets partition:
echo esptool.py --chip esp32s3 -p COM3 -b 921600 write_flash ...
echo.
pause
