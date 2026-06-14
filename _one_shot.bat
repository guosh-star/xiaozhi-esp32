@echo off
call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57

cd /d C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32

echo ========== 第一步：打开 menuconfig 修改唤醒词设置 ==========
echo.
echo 请在 menuconfig 中操作：
echo   Xiaozhi Assistant Configuration
echo    → Wake Word Selection
echo     → 把 [*] Wake Word Disabled 取消（按空格）
echo     → 选择 [*] ESP Wake Word （按空格选中）
echo    → Wake Word Model
echo     → [*] wn9_nihaoxiaozhi_tts （唤醒词：你好小智，带TTS）
echo    → Multinet Model Selection
echo     → [*] Multinet7 Quantized
echo.
echo 然后按 S 保存，按回车确认，再按 Q 退出。
echo.
pause
idf.py menuconfig

echo.
echo ========== 第二步：清理并重新编译 ==========
idf.py fullclean
idf.py build

if %ERRORLEVEL% NEQ 0 (
    echo 编译失败，请检查错误信息！
    pause
    exit /b 1
)

echo.
echo ========== 第三步：烧录固件到 COM3 ==========
echo 请确保：
echo  1. 按住 BOOT 按钮不松
echo  2. 按一下 EN/RST 按钮
echo  3. 松开 BOOT 按钮
echo  4. 然后按任意键继续...
echo.
echo 如果连不上，按 Ctrl+C 退出，手动按住 BOOT 再试
pause

idf.py -p COM3 -b 921600 flash

if %ERRORLEVEL% NEQ 0 (
    echo 烧录失败！请检查：
    echo  1. USB 线是否插好
    echo  2. 是否按住 BOOT 按钮
    echo  3. COM3 是否正确（可在设备管理器查看）
    pause
    exit /b 1
)

echo.
echo ========== 完成！ ==========
echo 烧录成功！
echo 请拔掉 USB 重新插上，等待设备启动。
echo.
echo 启动后应该能看到 OLED 显示内容，并有开机提示音。
echo 如果没有声音可以试试调大音量。
echo.
pause
