@echo off
call C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat
cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32
idf.py build
