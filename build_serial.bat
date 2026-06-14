@echo off
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat
echo === BUILDING ===
idf.py build
