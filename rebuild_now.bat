@echo off
REM Clean build script
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
REM Clear corrupt ccache
if exist "%USERPROFILE%\.ccache" del /s /q "%USERPROFILE%\.ccache\*" >nul 2>&1
set CCACHE_DISABLE=1
idf.py build
