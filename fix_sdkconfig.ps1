# 修正 sdkconfig 脚本
# 1. 改板型为 BREAD_COMPACT_WIFI (ESP32-S3)
# 2. 启用自定义唤醒词 "bu gu niao"
# 3. 启用 MultiNet 模型
# 4. 修复Flash大小为16MB

$sdkconfig = "C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\sdkconfig"
$content = Get-Content $sdkconfig -Raw

# --- 板型设置 ---
# 禁用旧的ESP32板型，启用WiFi板型
$content = $content -replace "CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32=y", "# CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32 is not set"
$content = $content -replace "# CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI is not set", "CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y"

# --- 目标芯片 (需要是ESP32-S3) ---
$content = $content -replace "CONFIG_IDF_TARGET=`"`"", "CONFIG_IDF_TARGET=`"esp32s3`""

# --- Flash大小 (16MB) ---
$content = $content -replace "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y", "# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set"
$content = $content -replace "# CONFIG_ESPTOOLPY_FLASHSIZE_16MB is not set", "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"
$content = $content -replace 'CONFIG_ESPTOOLPY_FLASHSIZE="4MB"', 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"'

# --- 分区表 (16MB custom wakeword版) ---
$content = $content -replace 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/4m.csv"', 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v1/16m_custom_wakeword.csv"'
$content = $content -replace 'CONFIG_PARTITION_TABLE_FILENAME="partitions/v2/4m.csv"', 'CONFIG_PARTITION_TABLE_FILENAME="partitions/v1/16m_custom_wakeword.csv"'

# --- 唤醒词 ---
# 禁用唤醒词禁用
$content = $content -replace "CONFIG_WAKE_WORD_DISABLED=y", "# CONFIG_WAKE_WORD_DISABLED is not set"
# 启用自定义唤醒词
$content = $content -replace "# CONFIG_USE_CUSTOM_WAKE_WORD is not set", "CONFIG_USE_CUSTOM_WAKE_WORD=y"
$content = $content -replace 'CONFIG_CUSTOM_WAKE_WORD=""', 'CONFIG_CUSTOM_WAKE_WORD="bu gu niao"'
$content = $content -replace 'CONFIG_CUSTOM_WAKE_WORD_DISPLAY=""', 'CONFIG_CUSTOM_WAKE_WORD_DISPLAY="布谷鸟"'
$content = $content -replace "CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=", "CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20`n"
# 如果已有的话，直接设
if ($content -notmatch "CONFIG_CUSTOM_WAKE_WORD=") {
    $content = $content -replace "(CONFIG_USE_CUSTOM_WAKE_WORD=y)", "`$1`r`nCONFIG_CUSTOM_WAKE_WORD=""bu gu niao""`r`nCONFIG_CUSTOM_WAKE_WORD_DISPLAY=""布谷鸟""`r`nCONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20"
}
# 发送唤醒词数据
if ($content -notmatch "CONFIG_SEND_WAKE_WORD_DATA=y") {
    $content = $content -replace "(CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20)", "`$1`r`nCONFIG_SEND_WAKE_WORD_DATA=y"
}

# --- MultiNet (中文命令词识别) ---
$content = $content -replace "CONFIG_SR_MN_CN_NONE=y", "# CONFIG_SR_MN_CN_NONE is not set"
$content = $content -replace "# CONFIG_SR_MN_CN_MULTINET7_CN is not set", "CONFIG_SR_MN_CN_MULTINET7_CN=y"

# --- 启用WakeNet (使用已有模型) ---
$content = $content -replace "CONFIG_WAKE_WORD_DISABLED=y", "# CONFIG_WAKE_WORD_DISABLED is not set"

Set-Content $sdkconfig $content -NoNewline

Write-Host "✅ sdkconfig 已更新！"
Write-Host "板型: BREAD_COMPACT_WIFI (ESP32-S3)"
Write-Host "Flash: 16MB"
Write-Host "分区表: 16m_custom_wakeword.csv"
Write-Host "唤醒词: bu gu niao (布谷鸟)"
Write-Host "MultiNet: MULTINET7_CN"
