@echo off
call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57

echo ========== Flashing MANUFACTURER firmware ==========
echo.

C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.4\components\esptool_py\esptool\esptool.py ^
 --chip esp32s3 -p COM3 -b 921600 --before=default_reset --after=hard_reset ^
 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB ^
 0x0 D:\BaiduNetdiskDownload\面包板版本（小智ai）\刷机固件\merged-binary.bin

echo ========== EXIT CODE: %ERRORLEVEL% ==========
if %ERRORLEVEL% EQU 0 (
    echo.
    echo DONE! Unplug USB and replug.
)
pause
