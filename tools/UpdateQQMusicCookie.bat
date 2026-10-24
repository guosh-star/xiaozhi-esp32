@echo off
chcp 65001 >nul
title Update QQ Music Cookie
cd /d "%~dp0.."
"C:\Program Files\nodejs\node.exe" tools\qq_cookie_sync.js
pause
