@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%"
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat >nul 2>&1
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
set CCACHE_DISABLE=1
idf.py build 2>&1
echo IDF_EXIT=%ERRORLEVEL%
