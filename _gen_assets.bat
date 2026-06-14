@echo off
call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57

cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32

echo ========== Generating assets.bin ==========
echo.

C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe scripts\build_default_assets.py ^
 --sdkconfig sdkconfig ^
 --builtin_text_font font_puhui_basic_14_1 ^
 --output build\assets.bin

echo ========== EXIT CODE: %ERRORLEVEL% ==========

echo.
echo ========== Checking generated files ==========
dir build\assets.bin 2>nul
dir build\srmodels\srmodels.bin 2>nul

echo.
echo ========== DONE ==========
pause
