"""Fix all garbled + English comments in 5 part files.
Strategy: reverse mojibake (Latin-1→UTF-8), then translate English→Chinese."""
import os, re

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
DST = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

def fix_garbled_line(line):
    """Try to reverse mojibake: garbled text -> Latin-1 bytes -> UTF-8 decode"""
    try:
        # Take the raw string, encode as Latin-1, decode as UTF-8
        return line.encode('latin-1').decode('utf-8')
    except (UnicodeEncodeError, UnicodeDecodeError):
        return line  # Can't fix, leave as-is

# English → Chinese translation map for common patterns
EN_TO_CN = {
    # Part 1
    "// /stream?q=... -> /pcm?q=...": "// URL格式转换: /stream?q=... → /pcm?q=...",
    "// /opus?q=... -> /pcm?q=...": "// URL格式转换: /opus?q=... → /pcm?q=...",
    "// ---- Smooth ducking: fade music out when AI starts speaking ----": "// ---- 平滑降音: AI开始说话时淡出音乐 ----",
    "// Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring": "// 状态机: 0=空闲, 1=淡出中, 2=已降音, 3=恢复中",
    "// Apply gain + clip": "// 增益处理 + 限幅",
    " * @param url HTTP URL": " * @param url HTTP地址",
    "// Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz": "// 最坏情况MP3帧缓冲区 (1152立体声, 8000→24000Hz重采样)",
    "// ---- Smooth ducking: fade when AI starts speaking ----": "// ---- 平滑降音: AI说话时淡出音乐 ----",
    "// Step 1: stereo -> mono (average L/R)": "// 第1步: 立体声→单声道 (取左右声道平均值)",
    
    # Part 2
    "// Clear stale bg audio from previous session to prevent startup noise burst": "// 清除上次会话残留的背景音频, 防止开机爆音",
    "// === Serial fallback ===": "// === 串口回退 ===",
    "// HACK: Prevent audio watchdog timeout during long downloads": "// HACK: 防止长时间下载时音频看门狗超时",
    "// === Detect format: /opus uses OGG demuxer + main audio pipeline ===": "// === 格式检测: /opus用OGG解复用+主音频管线 ===",
    "// === /pcm uses raw bytes pushed to background ring buffer ===": "// === /pcm用原始字节推送到背景环形缓冲 ===",
    "// ============ Opus path: OGG demux + PushPacketToDecodeQueue ============": "// ============ Opus路径: OGG解复用+推送到解码队列 ============",
    "// ============ PCM path (legacy): raw s16le PushBackgroundAudio ============": "// ============ PCM路径(旧): 原始s16le推送到背景音频 ============",
    "// === Pre-buffer phase: fill ring buffer before enabling drain ===": "// === 预缓冲阶段: 先填充环形缓冲再启用输出 ===",
    "// Set gain based on current AI state before enabling drain": "// 启用输出前根据当前AI状态设置增益",
    "// (prevents full-volume music + TTS overlap noise)": "// (防止全音量音乐与TTS重叠产生噪音)",
    "// Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)": "// 仅在AI实际说话时降音; 聆听保持满音量 (用户需求 2026-07-19)",
    "// Set gain even on timeout otherwise stays at 0.001f": "// 超时也设置增益, 否则卡在0.001f",
    "// === Main download push loop ===": "// === 主下载推送循环 ===",
    "// Listening keeps music at 100%; duck to 50% only while AI speaks/connects": "// 聆听时音乐保持100%; AI说话/连接时才降到50%",
    "// When AI is speaking, pause TCP download to free WiFi airtime for": "// AI说话时暂停TCP下载, 释放WiFi空口时间给",
    "// UDP audio packets (prevents WiFi buffer starvation TTS stutter).": "// UDP音频包 (防止WiFi缓冲区饥饿导致TTS卡顿)",
    "// Only pause if buffer sufficient to ride through typical AI reply.": "// 仅在缓冲足够支撑典型AI回复时长时才暂停",
    "// Server closed connection gracefully": "// 服务器正常关闭连接",
    "// read < 0: timeout or transient error": "// read < 0: 超时或瞬时错误",
    "// Wait 1s before retry WiFi may be reconnecting after AI conversation": "// 等1秒重试, AI对话后WiFi可能正在重连",
    "// Refresh power save in case it was changed by channel close": "// 刷新省电模式, 防止被通道关闭修改",
    "// Diagnostic: log buffer fill + download rate every 8s": "// 诊断: 每8秒记录缓冲填充+下载速率",
    "// Fix(2026-07-19): idle transition during music skips threshold restore": "// 修复(2026-07-19): 音乐中空闲转换跳过阈值恢复",
    "// (IsBgAudioActive guard), leaving 0.30 stuck. Restore sensitive 0.02 here.": "// (IsBgAudioActive守卫), 导致卡在0.30。在此恢复灵敏的0.02",
    "// play single track - manage mute here": "// 播放单曲 - 在此管理静音",
    "// play bell chime using short PCM bell sound (~0.5s each)": "// 用短PCM钟声(~0.5s每个)播放报时钟声",
    "// pause ~1s between strikes for natural clock sound": "// 每敲之间停~1秒, 模拟自然钟声",
    "// Decoder buffered all input; no progress possible, stop": "// 解码器已缓存全部输入; 无法继续, 停止",
    "*out_buf = buf;": "",
    "*out_samples = written;": "",
    
    # Part 3
    "// Mix dog bark on top of existing bg audio (overlap, not replace)": "// 将狗叫叠加混入现有背景音频 (叠加, 不替换)",
    "// Bg music may still be fading in while AI speaks: wait up to 5s for it": "// 背景音乐可能还在淡入中(AI同时说话): 最多等5秒",
    "// Create PerformanceTask on Core 1": "// 在Core 1创建报时任务",
    "// Wait for AI to finish speaking (max 5s)": "// 等AI说完话 (最长5秒)",
    "// Abort AI speech if still talking": "// 如果还在说话则中止AI",
    "// Wait for bg audio to start": "// 等待背景音频启动",
    "// Wait for dance intro to finish": "// 等待舞蹈开场完成",
    "// Wait for music to end": "// 等待音乐结束",
}

UNTRANSLATED = set()

def translate_english(text):
    """Translate English comment to Chinese"""
    t = text.strip()
    if t in EN_TO_CN:
        result = EN_TO_CN[t]
        if result:
            return result
        return None  # delete this line
    
    # Try prefix matching for common patterns
    UNTRANSLATED.add(t)
    return None  # Keep as-is for now

counts = {'garbled_fixed': 0, 'english_fixed': 0, 'english_kept': 0}

for p in parts:
    path = os.path.join(DST, p)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    modified = False
    new_lines = []
    
    for line in lines:
        stripped = line.strip()
        original = line
        
        # Step 1: try to fix garbled
        # Only fix comment lines with garbled content
        if stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*'):
            # Check if garbled (has non-ASCII, non-Chinese chars)
            has_non_ascii = any(ord(c) > 127 for c in stripped)
            has_cjk = any(0x4e00 <= ord(c) <= 0x9fff for c in stripped)
            
            if has_non_ascii and not has_cjk:
                # Try Latin-1 → UTF-8 fix
                fixed = fix_garbled_line(line)
                if fixed != line:
                    line = fixed
                    counts['garbled_fixed'] += 1
                    modified = True
        
        # Step 2: after garbled fix, translate English
        stripped2 = line.strip()
        if stripped2.startswith('//') or stripped2.startswith('*'):
            if all(ord(c) < 128 for c in stripped2):
                words = re.findall(r'[a-zA-Z]{3,}', stripped2)
                if words and not re.match(r'^(GPIO|PWM|LED|DMA|I2C|SPI|UART|MCPWM|LEDC|RMT|ADC|DAC|I2S|SDIO|HTTP|URL|MP3|PCM|TTS|NVS|RTC|LDR|OGG|OPUS|WAV|ID3|MDCT|SDMMC|JSON|API|NTP|SNTP|LWIP|ESP|IDF|CPU|RAM|ROM)_', stripped2):
                    # Try translation
                    translated = translate_english(line)
                    if translated:
                        line = translated
                        counts['english_fixed'] += 1
                        modified = True
                    else:
                        counts['english_kept'] += 1
        
        new_lines.append(line)
    
    if modified:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)
        print(f'{p}: {counts["garbled_fixed"]} garbled + {counts["english_fixed"]} English fixed')
    
    # Reset counts for next file
    counts['garbled_fixed'] = 0
    counts['english_fixed'] = 0
    counts['english_kept'] = 0

if UNTRANSLATED:
    print(f'\nUntranslated English ({len(UNTRANSLATED)}):')
    for t in sorted(UNTRANSLATED)[:30]:
        print(f'  {t[:100]}')

print('\nFix complete. Run _scan_comments.py to verify.')
