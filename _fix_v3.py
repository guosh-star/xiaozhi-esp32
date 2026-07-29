"""Fix garbled comments at byte level: Latin-1 bytes → UTF-8 reverse.
Also translate English comments to Chinese.
Works on raw bytes to avoid U+FFFD replacement issues."""
import os, re

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
DST = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

def fix_garbled_bytes(data):
    """Read raw bytes, try to reverse Latin-1 encoded UTF-8 mojibake"""
    try:
        # The file was written as valid UTF-8, but some Chinese was
        # double-encoded: UTF-8 bytes → Latin-1 chars → re-encoded as UTF-8
        # We need to: UTF-8 decode → Latin-1 encode → UTF-8 decode
        text = data.decode('utf-8', errors='replace')
        # Try to detect and fix mojibake in comment blocks
        lines = text.split('\n')
        fixed = []
        count = 0
        for line in lines:
            stripped = line.lstrip()
            if stripped.startswith('//') or stripped.startswith('*'):
                # Check if this line has garbled content
                try:
                    # Try: encode as Latin-1, decode as UTF-8
                    recovered = line.encode('latin-1').decode('utf-8')
                    if recovered != line:
                        # Check if recovery produced valid Chinese
                        cjk_count = sum(1 for c in recovered if 0x4e00 <= ord(c) <= 0x9fff)
                        if cjk_count > 0:
                            fixed.append(recovered)
                            count += 1
                            continue
                except (UnicodeEncodeError, UnicodeDecodeError):
                    pass
                # Also try CP1252 → UTF-8
                try:
                    recovered = line.encode('cp1252').decode('utf-8')
                    if recovered != line:
                        cjk_count = sum(1 for c in recovered if 0x4e00 <= ord(c) <= 0x9fff)
                        if cjk_count > 0:
                            fixed.append(recovered)
                            count += 1
                            continue
                except (UnicodeEncodeError, UnicodeDecodeError):
                    pass
                # Try raw bytes approach: find byte sequences that look like UTF-8
                try:
                    b = line.encode('utf-8')
                    decoded = b.decode('utf-8')
                    if decoded != line:
                        fixed.append(decoded)
                        count += 1
                        continue
                except:
                    pass
            fixed.append(line)
        return '\n'.join(fixed), count
    except Exception as e:
        return data.decode('utf-8', errors='replace'), 0

# English → Chinese translations
EN_TRANSLATIONS = {
    # === Part 1 ===
    "// /stream?q=... -> /pcm?q=...": "// URL转换: /stream?q=... → /pcm?q=...",
    "// /opus?q=... -> /pcm?q=...": "// URL转换: /opus?q=... → /pcm?q=...",
    "// ---- Smooth ducking: fade music out when AI starts speaking ----": "// ---- 平滑降音: AI开始说话时淡出音乐 ----",
    "// Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring": "// 状态机: 0=空闲, 1=淡出, 2=已降音, 3=恢复中",
    "// Apply gain + clip": "// 增益处理 + 限幅",
    "// Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz": "// 最坏情况MP3帧缓冲区 (1152立体声, 8000→24000Hz)",
    "// Step 1: stereo -> mono (average L/R)": "// 第1步: 立体声→单声道 (取左右均值)",
    "* @param url HTTP URL": "* @param url HTTP地址",
    "// ---- Smooth ducking: fade when AI starts speaking ----": "// ---- 平滑降音: AI说话时淡出 ----",

    # === Part 2 ===
    "// Clear stale bg audio from previous session to prevent startup noise burst": "// 清除上次残留背景音频, 防止开机爆音",
    "// === Serial fallback ===": "// === 串口回退 ===",
    "// HACK: Prevent audio watchdog timeout during long downloads": "// HACK: 长下载防止音频看门狗超时",
    "// === Detect format: /opus uses OGG demuxer + main audio pipeline ===": "// === 格式检测: /opus→OGG解复用+音频管线 ===",
    "// === /pcm uses raw bytes pushed to background ring buffer ===": "// === /pcm→原始字节推送到背景环形缓冲 ===",
    "// ============ Opus path: OGG demux + PushPacketToDecodeQueue ============": "// ============ Opus路径: OGG解复用+送入解码队列 ============",
    "// ============ PCM path (legacy): raw s16le PushBackgroundAudio ============": "// ============ PCM路径(旧): 原始s16le→背景音频 ============",
    "// === Pre-buffer phase: fill ring buffer before enabling drain ===": "// === 预缓冲阶段: 先填满环形缓冲再启用输出 ===",
    "// Set gain based on current AI state before enabling drain": "// 启动前根据AI状态设置增益",
    "// (prevents full-volume music + TTS overlap noise)": "// (防止音乐与TTS重叠噪音)",
    "// Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)": "// 仅在AI说话时降音; 聆听保持满音量 (2026-07-19)",
    "// Set gain even on timeout otherwise stays at 0.001f": "// 超时也设增益, 否则卡在0.001f",
    "// === Main download push loop ===": "// === 主下载推送循环 ===",
    "// Listening keeps music at 100%; duck to 50% only while AI speaks/connects": "// 聆听时音乐100%; AI说话时才降50%",
    "// When AI is speaking, pause TCP download to free WiFi airtime for": "// AI说话时停TCP下载, 释放WiFi空口给",
    "// UDP audio packets (prevents WiFi buffer starvation TTS stutter).": "// UDP音频包 (防WiFi缓冲不足导致TTS卡顿)",
    "// Only pause if buffer sufficient to ride through typical AI reply.": "// 仅在缓冲够撑典型AI回复时才暂停",
    "// Server closed connection gracefully": "// 服务器正常关闭连接",
    "// read < 0: timeout or transient error": "// 读取失败: 超时/瞬时错误",
    "// Wait 1s before retry WiFi may be reconnecting after AI conversation": "// 等1秒重试, AI对话后WiFi可能重连中",
    "// Refresh power save in case it was changed by channel close": "// 刷新省电模式(可能被通道关闭修改)",
    "// Diagnostic: log buffer fill + download rate every 8s": "// 诊断: 每8秒记缓冲状态+下载速率",
    "// Fix(2026-07-19): idle transition during music skips threshold restore": "// 修复(2026-07-19): 音乐时空闲切换跳过阈值恢复",
    "// (IsBgAudioActive guard), leaving 0.30 stuck. Restore sensitive 0.02 here.": "// (IsBgAudioActive守卫), 卡0.30。在此恢复0.02",
    "// play single track - manage mute here": "// 播放单曲 - 管理静音",
    "// play bell chime using short PCM bell sound (~0.5s each)": "// 短PCM钟声(~0.5s)播放报时",
    "// pause ~1s between strikes for natural clock sound": "// 每敲之间停~1秒, 模拟自然钟声",
    "// Decoder buffered all input; no progress possible, stop": "// 解码器已缓存全部; 无进展, 停止",
    "// Mix dog bark on top of existing bg audio (overlap, not replace)": "// 狗叫叠加混入背景音频 (叠加不替换)",
    "// Bg music may still be fading in while AI speaks: wait up to 5s for it": "// 背景音乐可能还在淡入: 最多等5秒",
    
    # === Part 3 ===
    "// Create PerformanceTask on Core 1": "// 在Core 1创建报时任务",
    "// Wait for AI to finish speaking (max 5s)": "// 等AI说完话 (最长5秒)",
    "// Abort AI speech if still talking": "// 还在说则中止AI",
    "// Wait for bg audio to start": "// 等背景音频启动",
    "// Wait for dance intro to finish": "// 等舞蹈开场完成",
    "// Wait for music to end": "// 等音乐结束",
    "// ---- ADC oneshot init ----": "// ---- ADC单次采样初始化 ----",
    "// --- Alarms ---": "// --- 闹钟 ---",
    "// --- Quiet Mode ---": "// --- 静音模式 ---",
    "// --- Hourly Performance Toggle ---": "// --- 整报时演出开关 ---",
    "// --- Music / Show ---": "// --- 音乐/演出 ---",
    "// --- Hardware (wiring later) ---": "// --- 硬件 (后续接线) ---",
    "// --- Cantonese lookup (2026-07-19) ---": "// --- 粤语查询 (2026-07-19) ---",
    
    # === Part 4: DogShow steps ===
    "// 1. Water wheel on": "// 1. 开水车",
    "// 2. Open door": "// 2. 开门",
    "// 3. Dog tail out: 180->20, 1200ms": "// 3. 狗尾伸出: 180→20°, 1200ms",
    "// 4. Dog forward": "// 4. 狗前进",
    "// 5. Bark #1": "// 5. 狗叫第1声",
    "// 6. Tail to 0 deg": "// 6. 尾巴归位0°",
    "// 7. Wag 10s: 0<->60": "// 7. 摇尾10秒: 0↔60°",
    "// 8. Bark #2": "// 8. 狗叫第2声",
    "// 9. Tail back to 20 deg": "// 9. 尾巴回20°",
    "// 10. Dog reverse": "// 10. 狗后退",
    "// 11. Tail back to 180 deg": "// 11. 尾巴回180°",
    "// 12. Stop water wheel": "// 12. 关水车",
    "// 13. Close door": "// 13. 关门",
    "// : (board B M2 via violin_motor_)": "// : (B板M2通过violin_motor_)",

    # === Part 5 ===
    "// Mp3Player tracks playback state (stays true during AI speech ducking),": "// Mp3Player播放状态 (AI说话降音期间仍为true),",
    "// while IsBgAudioActive() may briefly drop when audio service clears buffers.": "// 而IsBgAudioActive()在音频服务清理缓冲时会短暂掉为false",
    "// Using both ensures music dance survives AI conversations.": "// 同时检查两者确保音乐舞蹈在AI对话期间不中断",
    "// For online /stream or /opus: url is already correct": "// 在线/stream或/opus: URL已经正确",
}

# Translate individual English lines in context
def translate_line(line):
    """Translate an English comment line to Chinese"""
    stripped = line.strip()
    
    # Exact match first
    if stripped in EN_TRANSLATIONS:
        prefix = line[:len(line) - len(line.lstrip())]
        translated = EN_TRANSLATIONS[stripped]
        if translated:
            return prefix + translated + '\n'
        return None  # delete
    
    # Pattern-based translations
    if stripped.startswith('*') or stripped.startswith('/*'):
        if '@brief' in stripped or '@param' in stripped or '@return' in stripped:
            return line  # Keep doxygen headers as-is for compatibility
    
    return line  # Keep as-is if no match

total_garb = 0
total_eng = 0

for p in parts:
    path = os.path.join(DST, p)
    
    # Step 1: fix garbled at byte level
    with open(path, 'rb') as f:
        raw = f.read()
    
    fixed_text, garb_count = fix_garbled_bytes(raw)
    total_garb += garb_count
    
    # Step 2: translate English
    lines = fixed_text.split('\n')
    result = []
    eng_count = 0
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith('//'):
            # Check if English
            if all(ord(c) < 128 for c in stripped):
                words = re.findall(r'[a-zA-Z]{3,}', stripped)
                if words:
                    translated = translate_line(line)
                    if translated is not None:
                        result.append(translated)
                    if translated != line:
                        eng_count += 1
                    continue
        result.append(line)
    
    if garb_count > 0 or eng_count > 0:
        with open(path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(result))
    
    total_eng += eng_count
    print(f'{p}: {garb_count} garbled fixed, {eng_count} English translated')

print(f'\nTotal: {total_garb} garbled, {total_eng} English')
