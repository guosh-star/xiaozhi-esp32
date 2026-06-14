
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.4\components\esptool_py\esptool\esptool.py ^
 --chip esp32s3 -p COM3 -b 921600 --before=default_reset --after=hard_reset ^
 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB ^
 0x10000 build\assets.bin

echo ========== EXIT CODE: %ERRORLEVEL% ==========
if %ERRORLEVEL% EQU 0 (
    echo.
    echo SUCCESS! Unplug and replug the device to restart.
)
pause
