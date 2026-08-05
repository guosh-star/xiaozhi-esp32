@echo off
REM ============================================================
REM Prebuild check: verify stable config files haven't changed
REM If any check fails, the file will be restored from backup
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
echo.
if !RESTORE! equ 1 (
  echo One or more config files were restored from backup.
  echo Delete managed_components and retry build.
)
endlocal & exit /b %RESTORE%
