@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1;%PATH%"
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build
ninja 2>&1
