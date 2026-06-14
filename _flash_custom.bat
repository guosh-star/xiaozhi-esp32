@echo off
call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57

cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32

echo ========== Running idf.py flash ==========
idf.py -p COM3 -b 921600 flash
echo ========== EXIT CODE: %ERRORLEVEL% ==========
pause
