#!/usr/bin/env python3
"""Fill all empty comments with proper Chinese annotations based on code context."""
import re

CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
with open(CC, 'r', encoding='utf-8') as f:
    lines = f.read().splitlines()

# Map of code patterns -> comment replacements
# Key: regex pattern to match context
# Value: list of (line_offset_from_match, comment_text)
rules = []

def rule(pattern, offset, text):
    rules.append((re.compile(pattern), offset, text))

# ===== FILE HEADER =====
rule(r'^// ============================================$', 1, '// 布谷鸟钟控制器 (Cuckoo Controller)')
rule(r'^// 布谷鸟钟控制器', 1, '//')
rule(r'^// 布谷鸟钟控制器', 2, '// 功能概要：')
rule(r'^// 功能概要', 1, '// - 电机控制：4路直流电机 + 1路摆动电机 + 水轮电机')
rule(r'^// - 电机控制', 1, '// - 尾巴控制：小提琴电机 + 狗尾舵机摆动')
rule(r'^// - 尾巴控制', 1, '// - LED 闪烁：2路 LED 开/关控制')
rule(r'^// - LED 闪烁', 1, '// - 音频播放：MP3/Opus/PCM 解码，HTTP 下载，Ducking 智能混音')
rule(r'^// - 音频播放', 1, '// - 整点/半点报时：门扇控制 + 小鸟跳跃 + 音乐播放 + 灯光')
rule(r'^// - 整点/半点报时', 1, '// - 综合表演：舞蹈旋转 + 小提琴 + 小狗 + 水车 + 铃铛 + LED')
rule(r'^// - 综合表演', 1, '// - 角色定制 MCP 工具：DogShow、LindaShow、GardenShow')
rule(r'^// - 角色定制', 1, '// - 闹钟功能：NVS 持久化，最多5组，每日重复')
rule(r'^// - 闹钟功能', 1, '// - 静音模式：22:00-6:00 夜间静音')
rule(r'^// - 静音模式', 1, '// - 在线音乐播放：QQ 音乐代理，Opus/PCM 格式下载')
rule(r'^// - 在线音乐播放', 1, '// - MCP 命令注册：21条指令，供 AI 大模型调用')
rule(r'^// - MCP 命令注册', 1, '//')
rule(r'^// - MCP 命令注册', 2, '// 双核架构：')
rule(r'^// 双核架构', 1, '// - Core 0: AI 语音（唤醒词、TTS、Opus编解码）')
rule(r'^// - Core 0', 1, '// - Core 1: 钟控（cuckoo_clock_task、250ms tick、闹钟和报时）')
rule(r'^// - Core 1', 1, '// - 表演/报时通过 xTaskCreatePinnedToCore 到 Core 1 执行')

# ===== ConvertToPcmUrl =====
rule(r'^//$', 0, '// URL 转换工具：将 /stream 或 /opus URL 转换为 /pcm 降低解码负载')
# Need context: this empty // must be followed by "static void ConvertToPcmUrl"
rule(r'^static void ConvertToPcmUrl', -1, '// URL 转换工具：将 /stream 或 /opus URL 转换为 /pcm 降低解码负载')
rule(r'^    //$', 0, '    // 检查 URL 是否超出缓冲区上限')
rule(r'^        //$', 0, '        // "/stream" = 7字符, 用 "/pcm"(4字符) 覆盖')
rule(r'^        // "/opus" =', -1, '        // "/opus" = 6字符, 用 "/pcm"(4字符) 覆盖')
rule(r'^    //$', 0, '    // 检查 URL 是否超出缓冲区上限')

# Process rules
fixed = 0
for i, line in enumerate(lines):
    s = line.strip()
    if s not in ['//', '/**', '*', ' * ']:
        continue
    # Try each rule
    indent = line[:len(line) - len(line.lstrip())]
    for pattern, offset, text in rules:
        check_line = i + offset
        if check_line < 0 or check_line >= len(lines):
            continue
        if pattern.match(lines[check_line].strip()):
            # Verify target line is empty comment
            target_line = lines[i].strip()
            if target_line in ['//', '/**', '*', ' * ']:
                lines[i] = indent + text
                fixed += 1
                break

with open(CC, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))

rem = sum(1 for l in lines if l.strip() in ['//', '/**', '*', ' * '])
print(f'Fixed: {fixed}, Remaining: {rem}, Lines: {len(lines)}')
