"""Final fix: 3 remaining garbled lines + 136 English comments."""
import os, sys
sys.stdout.reconfigure(encoding='utf-8')

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
D = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')

# 1. Fix 3 remaining garbled lines
REMAINING_GARB = {
    ('cuckoo_part2.cc', 120): "// 原始BSD socket",
    ('cuckoo_part2.cc', 512): "// 原来 while 等待排空会导致 MusicDanceTick 继续跑 ~5 秒（79K 样本 / 16kHz）",
    ('cuckoo_part3.cc', 784): "* @param hour 小时 (0-23)",
}

for (part, ln), correct in REMAINING_GARB.items():
    path = os.path.join(D, part)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    idx = ln - 1
    orig = lines[idx]
    indent = orig[:len(orig)-len(orig.lstrip())]
    lines[idx] = indent + correct + '\n'
    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print(f'Fixed {part} L{ln}')

# 2. Translate English comments
EN_MAP = {
    "// ---- Smooth ducking: fade music out when AI starts speaking ----": "// ---- 平滑降音: AI开始说话时淡出音乐 ----",
    "// Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring": "// 状态机: 0=空闲, 1=淡出中, 2=已降音, 3=恢复中",
    "// Apply gain + clip": "// 增益处理 + 限幅",
    "// Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz": "// 最坏情况MP3帧缓冲 (1152立体声, 8000→24000Hz重采样)",
    "// Step 1: stereo -> mono (average L/R)": "// 第1步: 立体声→单声道 (取左右均值)",
    "// ---- Smooth ducking: fade when AI starts speaking ----": "// ---- 平滑降音: AI说话时淡出 ----",
    "* @param url HTTP URL": "* @param url HTTP地址",
    
    "// Clear stale bg audio from previous session to prevent startup noise burst": "// 清除上次残留背景音频, 防止开机爆音",
    "// === Serial fallback ===": "// === 串口回退 ===",
    "// HACK: Prevent audio watchdog timeout during long downloads": "// HACK: 长下载防止音频看门狗超时",
    "// === Detect format: /opus uses OGG demuxer + main audio pipeline ===": "// === 格式检测: /opus→OGG解复用+音频主管线 ===",
    "// === /pcm uses raw bytes pushed to background ring buffer ===": "// === /pcm→原始字节→背景环形缓冲 ===",
    "// ============ Opus path: OGG demux + PushPacketToDecodeQueue ============": "// ============ Opus路径: OGG解复用+送入解码队列 ============",
    "// ============ PCM path (legacy): raw s16le PushBackgroundAudio ============": "// ============ PCM路径(旧): 原始s16le→背景音频 ============",
    "// === Pre-buffer phase: fill ring buffer before enabling drain ===": "// === 预缓冲阶段: 先填满环形缓冲再启用输出 ===",
    "// Set gain based on current AI state before enabling drain": "// 启用输出前根据AI状态设置增益",
    "// (prevents full-volume music + TTS overlap noise)": "// (防止全音量音乐+TTS重叠噪音)",
    "// Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)": "// 仅AI说话时降音; 聆听时保持满音量 (2026-07-19需求)",
    "// Set gain even on timeout otherwise stays at 0.001f": "// 超时也设增益, 否则卡在0.001f",
    "// === Main download push loop ===": "// === 主下载推送循环 ===",
    "// Listening keeps music at 100%; duck to 50% only while AI speaks/connects": "// 聆听时音乐100%; AI说话/连接时才降50%",
    "// When AI is speaking, pause TCP download to free WiFi airtime for": "// AI说话时暂停TCP下载, 释放WiFi空口给",
    "// UDP audio packets (prevents WiFi buffer starvation TTS stutter).": "// UDP音频包 (防止WiFi缓冲不足导致TTS卡顿)",
    "// Only pause if buffer sufficient to ride through typical AI reply.": "// 仅缓冲足够撑过典型AI回复时才暂停",
    "// Server closed connection gracefully": "// 服务器正常关闭连接",
    "// read < 0: timeout or transient error": "// 读取失败: 超时或瞬时错误",
    "// Wait 1s before retry WiFi may be reconnecting after AI conversation": "// 等1秒后重试 (AI对话后WiFi可能重连中)",
    "// Refresh power save in case it was changed by channel close": "// 刷新省电模式 (防止被通道关闭修改)",
    "// Diagnostic: log buffer fill + download rate every 8s": "// 诊断: 每8秒记录缓冲填充+下载速率",
    "// Fix(2026-07-19): idle transition during music skips threshold restore": "// 修复(2026-07-19): 音乐中空闲切换跳过阈值恢复",
    "// (IsBgAudioActive guard), leaving 0.30 stuck. Restore sensitive 0.02 here.": "// (IsBgAudioActive守卫), 导致卡0.30, 在此恢复灵敏的0.02",
    "// play single track - manage mute here": "// 播放单曲 - 在此管理静音",
    "// play bell chime using short PCM bell sound (~0.5s each)": "// 用短PCM钟声(~0.5s)播放报时钟声",
    "// pause ~1s between strikes for natural clock sound": "// 每敲之间停约1秒, 模拟自然钟声",
    "// Decoder buffered all input; no progress possible, stop": "// 解码器已缓冲全部输入; 无法继续, 停止",
    "*out_buf = buf;": "*out_buf = buf;",
    "*out_samples = written;": "*out_samples = written;",
    "// Mix dog bark on top of existing bg audio (overlap, not replace)": "// 狗叫叠加混入现有背景音频 (叠加不替换)",
    "// Bg music may still be fading in while AI speaks: wait up to 5s for it": "// 背景音乐可能还在淡入中 (AI同时说话): 最多等5秒",
    
    "// Create PerformanceTask on Core 1": "// 在Core 1创建报时任务",
    "// Wait for AI to finish speaking (max 5s)": "// 等AI说完话 (最长5秒)",
    "// Abort AI speech if still talking": "// 还在说则中止AI",
    "// Wait for bg audio to start": "// 等待背景音频启动",
    "// Wait for dance intro to finish": "// 等待舞蹈开场完成",
    "// Wait for music to end": "// 等待音乐结束",
    "// ---- ADC oneshot init ----": "// ---- ADC单次采样初始化 ----",
    "// --- Alarms ---": "// --- 闹钟 ---",
    "// --- Quiet Mode ---": "// --- 静音模式 ---",
    "// --- Hourly Performance Toggle ---": "// --- 整点演出开关 ---",
    "// --- Music / Show ---": "// --- 音乐/演出 ---",
    "// --- Hardware (wiring later) ---": "// --- 硬件 (后续接线) ---",
    "// --- Cantonese lookup (2026-07-19) ---": "// --- 粤语查询 (2026-07-19) ---",
    
    "// 1. Water wheel on": "// 1. 开水车",
    "// 2. Open door": "// 2. 开门",
    "// 3. Dog tail out: 180->20, 1200ms": "// 3. 狗尾伸出: 180→20度, 1200ms",
    "// 4. Dog forward": "// 4. 狗前进",
    "// 5. Bark #1": "// 5. 狗叫第1声",
    "// 6. Tail to 0 deg": "// 6. 尾巴归位0度",
    "// 7. Wag 10s: 0<->60": "// 7. 摇尾10秒: 0↔60度",
    "// 8. Bark #2": "// 8. 狗叫第2声",
    "// 9. Tail back to 20 deg": "// 9. 尾巴回20度",
    "// 10. Dog reverse": "// 10. 狗后退",
    "// 11. Tail back to 180 deg": "// 11. 尾巴回180度",
    "// 12. Stop water wheel": "// 12. 关水车",
    "// 13. Close door": "// 13. 关门",
    "// : (board B M2 via violin_motor_)": "// : (B板M2通过violin_motor_)",
    
    "// Mp3Player tracks playback state (stays true during AI speech ducking),": "// Mp3Player播放状态 (AI说话降音期间仍为true),",
    "// while IsBgAudioActive() may briefly drop when audio service clears buffers.": "// 而IsBgAudioActive()在音频服务清理缓冲时会瞬间掉false",
    "// Using both ensures music dance survives AI conversations.": "// 同时检查两者确保音乐舞蹈在AI对话中不中断",
    "// For online /stream or /opus: url is already correct": "// 在线/stream或/opus: URL已正确",
    "// Save stop_requested_ state for later custom messages": "// 保存stop_requested_状态供后续自定义消息使用",
    "// Check LED state after AI stops (for kids_dance_ restart)": "// AI停止后检查LED状态 (用于重新启动kids_dance_)",
}

parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']
en_total = 0
for p in parts:
    path = os.path.join(D, p)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    modified = False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped in EN_MAP:
            prefix = line[:len(line)-len(stripped)]
            new_text = EN_MAP[stripped]
            if new_text != stripped:
                lines[i] = prefix + new_text + '\n'
                en_total += 1
                modified = True
    
    if modified:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(lines)

print(f'Translated {en_total} English comments')

# 3. Verify
print('\n--- Final verification ---')
os.system(f'python {os.path.join(BASE, "_scan_comments.py")}')

for p in parts:
    path = os.path.join(D, p)
    with open(path, 'r', encoding='utf-8') as f:
        n = len(f.readlines())
    print(f'{p}: {n} lines')
