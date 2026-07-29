@echo off
REM ============================================================
REM Prebuild check: verify stable config files haven't changed
REM If any check fails, the file will be restored from backup
REM ============================================================
setlocal enabledelayedexpansion
set RESTORE=0
echo Checking idf_component.yml...
certutil -hashfile "%~dp0main\idf_component.yml" SHA256 | find /i "F7B20ACA8EFFB10515B98BADF08634A173489938AE5735B98682CBEFB79F7369" > nul
if !errorlevel! neq 0 (
  echo   [idf_component.yml] CHANGED - restoring from backup
  copy /y "%~dp0memory\backups\stable-config\idf_component.yml" "%~dp0main\idf_component.yml" > nul
  set RESTORE=1
) else (
  echo   [idf_component.yml] OK
)
echo.
echo Checking dependencies.lock...
certutil -hashfile "%~dp0dependencies.lock" SHA256 | find /i "E0787431AB9B0C3E1AF5796FFC8C7410D24CD72C0D51111B78129BB9E418291D" > nul
if !errorlevel! neq 0 (
  echo   [dependencies.lock] CHANGED - restoring from backup
  copy /y "%~dp0memory\backups\stable-config\dependencies.lock" "%~dp0dependencies.lock" > nul
  set RESTORE=1
) else (
  echo   [dependencies.lock] OK
)
echo.
echo Checking sdkconfig...
certutil -hashfile "%~dp0sdkconfig" SHA256 | find /i "CC082BC4E04DFF16E716AEEC09F0C4C876E4CA94C87DFA82CF986B401560BADF" > nul
if !errorlevel! neq 0 (
  echo   [sdkconfig] CHANGED - restoring from backup
  copy /y "%~dp0memory\backups\stable-config\sdkconfig" "%~dp0sdkconfig" > nul
  set RESTORE=1
) else (
  echo   [sdkconfig] OK
)
echo.
if !RESTORE! equ 1 (
  echo One or more config files were restored from backup.
  echo Delete managed_components and retry build.
)
endlocal
exit /b 0
