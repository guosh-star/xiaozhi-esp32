@echo off
call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57

cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build

echo ========== Flashing partitions manually ==========
echo.

C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.4\components\esptool_py\esptool\esptool.py ^
 --chip esp32s3 -p COM3 -b 921600 --before=default_reset --after=hard_reset ^
 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB ^
 0x0000 bootloader\bootloader.bin ^
 0x8000 partition_table\partition-table.bin ^
 0x10000 xiaozhi.bin

echo ========== EXIT CODE: %ERRORLEVEL% ==========
if %ERRORLEVEL% EQU 0 (
    echo.
    echo SUCCESS! Unplug and replug the device to restart.
)
pause
