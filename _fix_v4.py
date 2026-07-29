"""Fix garbled + English comments: character-level mojibake recovery.
Handles mixed correct-Chinese + garbled lines."""
import os, re

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
DST = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

def try_recover_char(c):
    """Try to recover a single garbled character back to Chinese"""
    cp = ord(c)
    if cp < 128:
        return c
    if 0x4e00 <= cp <= 0x9fff:  # Already valid CJK
        return c
    if cp < 256:
        # Single byte that might be part of broken multi-byte
        try:
            b = bytes([cp])
            return b.decode('utf-8')
        except:
            return c
    return c

def fix_garbled_text(text):
    """Fix garbled Chinese by reversing double-encoding"""
    result = []
    i = 0
    while i < len(text):
        c = text[i]
        cp = ord(c)
        if cp < 128:
            result.append(c)
            i += 1
            continue
        if 0x4e00 <= cp <= 0x9fff:
            result.append(c)
            i += 1
            continue
        
        # Try to recover: collect consecutive non-CJK, non-ASCII chars
        # and try to decode them as Latin-1 bytes → UTF-8
        start = i
        while i < len(text):
            cp2 = ord(text[i])
            if cp2 >= 128 and cp2 < 0x4e00:
                i += 1
            else:
                break
        
        chunk = text[start:i]
        if chunk:
            # Try Latin-1 → UTF-8 recovery
            try:
                recovered = chunk.encode('latin-1').decode('utf-8')
                result.append(recovered)
            except:
                # Try CP1252
                try:
                    recovered = chunk.encode('cp1252').decode('utf-8')
                    result.append(recovered)
                except:
                    result.append(chunk)
        else:
            result.append(c)
            i += 1
    
    return ''.join(result)

EN_FULL = {
    # === P1 ===
    "// /stream?q=... -> /pcm?q=...": "// URL: /stream?q=... → /pcm?q=...",
    "// /opus?q=... -> /pcm?q=...": "// URL: /opus?q=... → /pcm?q=...",
    "// ---- Smooth ducking: fade music out when AI starts speaking ----": "// ---- 平滑降音: AI说话时淡出 ----",
    "// Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring": "// 状态机: 0=空闲, 1=淡出, 2=已降, 3=恢复",
    "// Apply gain + clip": "// 增益+限幅",
    "// Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz": "// 最坏MP3帧缓冲(1152立体声 8000→24000Hz)",
    "// Step 1: stereo -> mono (average L/R)": "// 第1步: 立体声→单声道(取均值)",
    " * @param url HTTP URL": " * @param url 地址",
    "// ---- Smooth ducking: fade when AI starts speaking ----": "// ---- 平滑降音: AI说话时淡出 ----",
    
    # === P2 ===
    "// Clear stale bg audio from previous session to prevent startup noise burst": "// 清除上次残留音频, 防开机爆音",
    "// === Serial fallback ===": "// === 串口回退 ===",
    "// HACK: Prevent audio watchdog timeout during long downloads": "// HACK: 长下载防看门狗超时",
    "// === Detect format: /opus uses OGG demuxer + main audio pipeline ===": "// === 格式: /opus→OGG解复用+主管线 ===",
    "// === /pcm uses raw bytes pushed to background ring buffer ===": "// === /pcm→原始字节→背景环形缓冲 ===",
    "// ============ Opus path: OGG demux + PushPacketToDecodeQueue ============": "// ============ Opus: OGG解复用+送解码队列 ============",
    "// ============ PCM path (legacy): raw s16le PushBackgroundAudio ============": "// ============ PCM(旧): 原始s16le→背景音频 ============",
    "// === Pre-buffer phase: fill ring buffer before enabling drain ===": "// === 预缓冲: 填满环形缓冲再启用输出 ===",
    "// Set gain based on current AI state before enabling drain": "// 启用前按AI状态设增益",
    "// (prevents full-volume music + TTS overlap noise)": "// (防全音量音乐+TTS重叠噪音)",
    "// Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)": "// 仅AI说话时降音; 聆听满音量(2026-07-19需求)",
    "// Set gain even on timeout otherwise stays at 0.001f": "// 超时也设增益, 防卡0.001f",
    "// === Main download push loop ===": "// === 主下载推送循环 ===",
    "// Listening keeps music at 100%; duck to 50% only while AI speaks/connects": "// 聆听100%; AI说话/连接时降50%",
    "// When AI is speaking, pause TCP download to free WiFi airtime for": "// AI说话时停TCP下载让WiFi给",
    "// UDP audio packets (prevents WiFi buffer starvation TTS stutter).": "// UDP音频包(防WiFi不足致TTS卡顿)",
    "// Only pause if buffer sufficient to ride through typical AI reply.": "// 仅缓冲够撑AI回复时才停",
    "// Server closed connection gracefully": "// 服务器正常关闭",
    "// read < 0: timeout or transient error": "// 读失败: 超时/瞬时错",
    "// Wait 1s before retry WiFi may be reconnecting after AI conversation": "// 等1秒重试(AI对话后WiFi可能重连)",
    "// Refresh power save in case it was changed by channel close": "// 刷新省电(可能被通道关闭改过)",
    "// Diagnostic: log buffer fill + download rate every 8s": "// 诊断: 每8秒记缓冲+速率",
    "// Fix(2026-07-19): idle transition during music skips threshold restore": "// 修复(2026-07-19): 音乐时闲切换跳阈值恢复",
    "// (IsBgAudioActive guard), leaving 0.30 stuck. Restore sensitive 0.02 here.": "// (IsBgAudioActive守卫)卡0.30, 在此恢复0.02",
    "// play single track - manage mute here": "// 单曲-管理静音",
    "// play bell chime using short PCM bell sound (~0.5s each)": "// 短PCM钟声(~0.5s)报时",
    "// pause ~1s between strikes for natural clock sound": "// 每敲停~1秒自然钟声",
    "// Decoder buffered all input; no progress possible, stop": "// 解码器已全缓冲; 无进展则停",
    "// Mix dog bark on top of existing bg audio (overlap, not replace)": "// 狗叫叠加混入背景音频(叠加不替换)",
    "// Bg music may still be fading in while AI speaks: wait up to 5s for it": "// 背景音乐可能仍在淡入中: 最多等5秒",
    
    # === P3 ===
    "// Create PerformanceTask on Core 1": "// Core 1创建报时任务",
    "// Wait for AI to finish speaking (max 5s)": "// 等AI说完(最长5秒)",
    "// Abort AI speech if still talking": "// 还在说则中止AI",
    "// Wait for bg audio to start": "// 等背景音频启动",
    "// Wait for dance intro to finish": "// 等舞蹈开场完",
    "// Wait for music to end": "// 等音乐结束",
    "// ---- ADC oneshot init ----": "// ---- ADC初始化 ----",
    "// --- Alarms ---": "// --- 闹钟 ---",
    "// --- Quiet Mode ---": "// --- 静音 ---",
    "// --- Hourly Performance Toggle ---": "// --- 整点演出开关 ---",
    "// --- Music / Show ---": "// --- 音乐/演出 ---",
    "// --- Hardware (wiring later) ---": "// --- 硬件(后续接线) ---",
    "// --- Cantonese lookup (2026-07-19) ---": "// --- 粤语查询(2026-07-19) ---",
    
    # === P4: DogShow ===
    "// 1. Water wheel on": "// 1. 开闸水车",
    "// 2. Open door": "// 2. 开门",
    "// 3. Dog tail out: 180->20, 1200ms": "// 3. 狗尾出: 180→20°",
    "// 4. Dog forward": "// 4. 狗前进",
    "// 5. Bark #1": "// 5. 叫第1声",
    "// 6. Tail to 0 deg": "// 6. 尾归0°",
    "// 7. Wag 10s: 0<->60": "// 7. 摇尾10秒: 0↔60°",
    "// 8. Bark #2": "// 8. 叫第2声",
    "// 9. Tail back to 20 deg": "// 9. 尾回20°",
    "// 10. Dog reverse": "// 10. 狗后退",
    "// 11. Tail back to 180 deg": "// 11. 尾回180°",
    "// 12. Stop water wheel": "// 12. 关水车",
    "// 13. Close door": "// 13. 关门",
    "// : (board B M2 via violin_motor_)": "// : (B板M2→violin_motor_)",

    # === P5 ===
    "// Mp3Player tracks playback state (stays true during AI speech ducking),": "// 播放状态(AI降音时仍true),",
    "// while IsBgAudioActive() may briefly drop when audio service clears buffers.": "// 而IsBgAudioActive清缓冲时会瞬间掉false",
    "// Using both ensures music dance survives AI conversations.": "// 双检保音乐舞蹈在AI对话中不中断",
    "// For online /stream or /opus: url is already correct": "// 在线/stream或/opus: URL已正确",
    "// Save stop_requested_ state for later custom messages": "// 保存stop_requested_状供后续消息用",
    "// Check LED state after AI stops (for kids_dance_ restart)": "// AI停后查LED(供重新开始kids_dance_)",
}

total_garb = 0
total_eng = 0

for p in parts:
    path = os.path.join(DST, p)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    lines = content.split('\n')
    new_lines = []
    garb_count = 0
    eng_count = 0
    
    for line in lines:
        orig = line
        stripped = line.lstrip()
        
        # Step 1: fix garbled (only comment lines)
        if stripped.startswith('//') or stripped.startswith('*'):
            # Fix garbled parts
            prefix = line[:len(line)-len(line.lstrip())]
            fixed_body = fix_garbled_text(stripped)
            if fixed_body != stripped:
                line = prefix + fixed_body
                garb_count += 1
        
        # Step 2: translate English
        stripped2 = line.lstrip()
        if stripped2 in EN_FULL:
            prefix = line[:len(line)-len(line.lstrip())]
            line = prefix + EN_FULL[stripped2]
            eng_count += 1
        
        new_lines.append(line)
    
    if garb_count or eng_count:
        with open(path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(new_lines))
    
    total_garb += garb_count
    total_eng += eng_count
    print(f'{p}: {garb_count} garbled + {eng_count} English fixed')

print(f'\nTotal: {total_garb} garbled, {total_eng} English')

# Re-scan
print('\n--- Re-scan ---')
import subprocess
subprocess.run(['python', '_scan_comments.py'], cwd=BASE, check=False)
