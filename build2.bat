@echo off
set "PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%"
cd /d "C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\build"
ninja xiaozhi.elf
pause
