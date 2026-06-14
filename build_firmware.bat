@echo off
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%

cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32

del build\esp-idf\main\CMakeFiles\__idf_main.dir\boards\cuckoo-clock\cuckoo_controller.cc.obj 2>nul
del build\esp-idf\main\CMakeFiles\__idf_main.dir\audio\codecs\box_audio_codec.cc.obj 2>nul

python C:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py build
