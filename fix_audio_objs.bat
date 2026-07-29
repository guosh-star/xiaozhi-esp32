@echo off
REM Fix: replace box_audio_codec and audio_service objs with old working versions
set AR="C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-ar.exe"
set OLD_MAIN="D:\文档\build\esp-idf\main\libmain.a"
set NEW_MAIN="build\esp-idf\main\libmain.a"
set TMP_DIR="%TEMP%\fix_audio_objs_%RANDOM%"

mkdir %TMP_DIR% 2>nul
pushd %TMP_DIR%
%AR% x %OLD_MAIN% box_audio_codec.cc.obj >nul 2>&1
%AR% x %OLD_MAIN% audio_service.cc.obj >nul 2>&1
popd

%AR% d %NEW_MAIN% box_audio_codec.cc.obj >nul 2>&1
%AR% d %NEW_MAIN% audio_service.cc.obj >nul 2>&1
%AR% r %NEW_MAIN% %TMP_DIR%\box_audio_codec.cc.obj >nul 2>&1
%AR% r %NEW_MAIN% %TMP_DIR%\audio_service.cc.obj >nul 2>&1

rmdir /s /q %TMP_DIR% 2>nul

echo === objs fixed. Now: idf.py build (link-only) ===
