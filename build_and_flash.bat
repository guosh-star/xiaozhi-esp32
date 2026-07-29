@echo off
set PYTHONIOENCODING=utf-8
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env

echo === Step 1: Build ===
idf.py build
if errorlevel 1 goto error

echo === Step 2: Fix audio objs ===
set AR=C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-ar.exe
set OLD_MAIN=D:\文档\build\esp-idf\main\libmain.a
set NEW_MAIN=build\esp-idf\main\libmain.a
set TMPDIR=%TEMP%\fix_audio_objs

mkdir %TMPDIR% 2>nul
pushd %TMPDIR%
%AR% x %OLD_MAIN% box_audio_codec.cc.obj
%AR% x %OLD_MAIN% audio_service.cc.obj
popd

%AR% d %NEW_MAIN% box_audio_codec.cc.obj
%AR% d %NEW_MAIN% audio_service.cc.obj
%AR% r %NEW_MAIN% %TMPDIR%\box_audio_codec.cc.obj
%AR% r %NEW_MAIN% %TMPDIR%\audio_service.cc.obj
rmdir /s /q %TMPDIR%

echo === Step 3: Link and flash ===
idf.py build flash
if errorlevel 1 goto error
goto end

:error
echo === BUILD FAILED ===
pause
exit /b 1

:end
echo === DONE ===
