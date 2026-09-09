@echo off
REM ============================================================
REM Prebuild check: verify stable config files haven't changed
REM Three files: idf_component.yml / dependencies.lock / sdkconfig
REM If any check fails, that file is restored from backup.
REM ============================================================
setlocal enabledelayedexpansion
set RESTORE=0

echo Checking idf_component.yml...
certutil -hashfile "%~dp0main\idf_component.yml" SHA256 | find /i "2349EE8C3CDA82A661849F73A8488E5EC75EDCF03322387F0101D273F62B2F00" > nul
if !errorlevel! neq 0 (
  echo   [idf_component.yml] CHANGED - restoring from backup
  copy /y "%~dp0memory\backups\stable-config\idf_component.yml" "%~dp0main\idf_component.yml" > nul
  set RESTORE=1
) else (
  echo   [idf_component.yml] OK
)

echo Checking dependencies.lock...
certutil -hashfile "%~dp0dependencies.lock" SHA256 | find /i "FDC5EE7DE754F2480F46F91A0F6F7C8A21CC795502D8AAAB81D5D21D47567412" > nul
if !errorlevel! neq 0 (
  echo   [dependencies.lock] CHANGED - restoring from backup
  copy /y "%~dp0memory\backups\stable-config\dependencies.lock" "%~dp0dependencies.lock" > nul
  set RESTORE=1
) else (
  echo   [dependencies.lock] OK
)

echo Checking sdkconfig...
certutil -hashfile "%~dp0sdkconfig" SHA256 | find /i "C6173BF08C9EB19E78B207A0056D1825E4399966E8720BF7829D9585D865365B" > nul
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
endlocal & exit /b %RESTORE%
