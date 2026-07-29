set PYTHONIOENCODING=utf-8
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\python_env\idf5.5_py3.11_env\Lib\site-packages\bin;%PATH%
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat > nul 2>&1
idf.py build
