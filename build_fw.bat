@echo off
REM ============================================================
REM Smart build: auto-detects whether fullclean is needed
REM ============================================================
setlocal enabledelayedexpansion

set PYTHONIOENCODING=utf-8
set CCACHE_DISABLE=1
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\python_env\idf5.5_py3.11_env\Lib\site-packages\bin;%PATH%
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat > nul 2>&1

call "%~dp0check_config.bat"
if !errorlevel! equ 1 (
    echo.
    echo ============================================================
    echo Config files CHANGED - fullclean required
    echo ============================================================
    rmdir /s /q "%~dp0managed_components"
    rmdir /s /q "%~dp0build"
    idf.py flash
) else (
    echo.
    echo Config files OK - building (fullclean to avoid stale cache)
    idf.py fullclean flash
)

endlocal
