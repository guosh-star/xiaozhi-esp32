@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;C:\Espressif\tools\dfu-util\0.11\bin;C:\Espressif\tools\esp-rom-elfs\20240305;%PATH%"
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
echo [1/2] Running fullclean...
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe "%IDF_PATH%\tools\idf.py" fullclean
echo.
echo [2/2] Running build...
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe "%IDF_PATH%\tools\idf.py" build
echo.
echo DONE.
