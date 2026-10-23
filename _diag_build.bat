@echo off
setlocal enabledelayedexpansion
set PYTHONIOENCODING=utf-8
set CCACHE_DISABLE=1
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\python_env\idf5.5_py3.11_env\Lib\site-packages\bin;%PATH%
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env

echo === step1: export.bat ===
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat
echo export exit code = %errorlevel%
echo.

echo === step2: which idf.py ===
where idf.py
echo where exit code = %errorlevel%
echo.

echo === step3: idf.py --version ===
idf.py --version
echo idf.py exit code = %errorlevel%
echo.

echo === step4: python version ===
python --version
echo.
endlocal
