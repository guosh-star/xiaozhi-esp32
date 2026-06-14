@echo off
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%
set PATH=C:\Espressif\tools\cmake\3.30.2\bin;%PATH%
set PATH=C:\Espressif\tools\ninja\1.12.1;%PATH%
set PATH=C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%PATH%
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
python C:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py build
