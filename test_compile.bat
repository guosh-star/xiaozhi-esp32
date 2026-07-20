@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\esp-cmake\1.1\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%"
set "CCACHE_DISABLE=1"
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build
C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-g++.exe @"C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build\toolchain\cxxflags" -c C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\application.cc -o test_app.o 2>&1
echo G++_EXIT=%ERRORLEVEL%
