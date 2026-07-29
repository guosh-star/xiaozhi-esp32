"""Final pass: translate remaining 79 English comments."""
import os, sys
sys.stdout.reconfigure(encoding='utf-8')

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
D = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')

en = {
    '// /stream?q=... -> /pcm?q=...': '// URL转换: /stream?q=... → /pcm?q=...',
    '// /opus?q=... -> /pcm?q=...': '// URL转换: /opus?q=... → /pcm?q=...',
    '// Music playing: mix cuckoo sound into bg audio (no interruption)': '// 音乐播放中: 布谷鸟声混入背景音频 (不中断)',
    '// LED (GPIO1KS8050 B, C, 5V)': '// LED指示灯 (GPIO1KS8050 B板, C口, 5V驱动)',
    '// or fall back to OutputRawPcm if bg audio is not active (e.g. DogShow)': '// 背景音频未激活时用OutputRawPcm (如DogShow)',
    '// to become active, otherwise the dance loop below exits instantly.': '// 变为活跃, 否则舞蹈循环立即退出',
    '// Phase 2: music + dance (bg audio, same path as start_show/LindaShow/GardenShow)': '// Phase 2: 音乐+舞蹈 (背景音频, 与演出相同路径)',
    '// Intro (door+dog, async, ~5s) must finish before the finale': '// 开场 (开门+狗出, 异步约5秒) 须在终曲前完成',
    '// retracts the dog / closes the door, or the sequences overlap.': '// 否则收狗/关门会与序列重叠',
    '// Stop water wheel + LEDs off': '// 关水车 + 灭LED',
    '// ====== Done ======': '// ====== 完成 ======',
    '// Check for duplicate - update existing': '// 检查重复闹钟 - 更新已有',
    '// Shift remaining alarms down': '// 后续闹钟下移',
    '// For one-shot alarms, disable after user stops it': '// 一次性闹钟: 用户停止后禁用',
    '// Stop any playing audio': '// 停止所有正在播放的音频',
    '// Play alarm ringtone x50 (2s each = 100s total ringing)': '// 播放闹铃50次 (每次2秒 = 共100秒)',
    '// First round: ramp volume 15%100% over first 10 calls (20s)': '// 第一轮: 前10次音量渐变 15%→100% (约20秒)',
    '// Snooze: wait 2 minutes, checking stopped_ every second': '// 贪睡: 等2分钟, 每秒检查stopped_标志',
    '// Cleanup': '// 清理',
    '// Find data chunk (may have fmt, LIST etc before it)': '// 找到data块 (前面可能有fmt/LIST等块)',
    '// DogShow path: no bg audio, use OutputRawPcm directly': '// DogShow路径: 无背景音频, 直接用OutputRawPcm',
    '// Push done: wait for ring buffer to drain, then clear bg audio flags': '// 推送完成: 等环形缓冲排空后清除背景音频标志',
    '// so show loops watching IsBgAudioActive() exit when music really ends.': '// 让监听IsBgAudioActive()的循环在音乐真正结束时退出',
    '// Wait for bg audio to start draining': '// 等背景音频开始输出',
    '// which the audio service mixes with TTS via ducking, no I2S conflict.': '// 音频服务通过ducking与TTS混音, 无I2S冲突',
    '// ====== Show start: LEDs on + water wheel ======': '// ====== 演出开始: 亮LED + 开水车 ======',
    '// Use PlayShowMusicBg (bg audio ring buffer) like LindaShow/GardenShow': '// 用PlayShowMusicBg (背景音频环形缓冲) 同LindaShow/GardenShow',
    '// Clean up bg audio AFTER finale completes (previously it was before, causing state gap)': '// 终曲完成后清理背景音频 (之前提前清理导致状态间隙)',
    '// ====== Stop water wheel + LEDs off ======': '// ====== 关水车 + 灭LED ======',
    '// Fix(2026-07-19): show runs while device stays idle, no state transition,': '// 修复(2026-07-19): 演出在idle态运行无状态转换,',
    '// clock task never restores 0.02 -> was stuck at 0.30 (hard to wake).': '// 钟控任务不恢复0.02 → 卡在0.30 (难以唤醒)',
    '// Track kids_active_ transition for mid-music changes': '// 跟踪kids_active_切换以便音乐中途变化',
    '// MusicDanceTick log muted to reduce noise (prints every 270ms)': '// MusicDanceTick日志静音 (每270ms)',
    '// Dog comes out when music plays &amp; kids active. NOT gated on': '// 音乐播放+活跃时狗出来, 不受',
    '// music_dance_enabled_ init block which can be skipped by stale state.': '// music_dance_enabled_初始块限制 (可被过期状态跳过)',
    '// Wait for dog_intro to finish before taking over servo/motor': '// 等dog_intro完成后接管舵机/电机',
    '// Dance motor: Phase 0-3 fwd 20ms, Phase 4-7 rev 20/21ms': '// 舞蹈电机: Phase 0-3 正转20ms, Phase 4-7 反转20/21ms',
    '// Guitar + Dog servos: sync to same phase rhythm': '// 小提琴+狗舵机: 与舞蹈同相节奏',
    '// Phase 0-3: forward beat, Phase 4-7: backward beat': '// Phase 0-3: 正拍动, Phase 4-7: 反拍动',
    "// Don't shut down during an active performance (ShowTask/KidsDanceShow)": '// 正在表演时不关 (ShowTask/KidsDanceShow进行中)',
    '// MusicDT-ELSE log muted to reduce noise (prints every 250ms)': '// MusicDT-ELSE日志静音 (每250ms)',
    '// Balance M1 motor: reverse must match forward': '// 平衡M1电机: 反转须匹配正转',
    '// Music ended: dog goes back, close door (only if kids were active)': '// 音乐结束: 狗回去关门 (仅kids活跃时)',
    '// Turn off LEDs when music ends': '// 音乐结束时灭LED',
    '// Re-enable motor power in case MusicDogOutro turned it off mid-way': '// 重新接通电机电源 (防MusicDogOutro半路关断)',
    '// First smooth return to 30deg, then back to home': '// 先平缓回30度, 再归位',
    "// Set kids_active_ NOW so MusicDanceTick doesn't enter else branch": '// 立即设kids_active_, 防MusicDanceTick进else分支',
    '// and trigger MusicDogOutro during the door-open delay below': '// 并在开门延迟期间触发MusicDogOutro',
    '// Visible dance: same big moves as hourly chime': '// 可见舞蹈: 与整点报时同的大幅动作',
    '// Music ended: full cleanup': '// 音乐结束: 完整清理',
    '// else: user stopped early, keep door open -> MusicDanceTick takes over': '// 否则: 提前停止, 保持开门 → MusicDanceTick接管',
    "// Safety: if music is playing and dog isn't out yet, force-create dog_intro.": '// 安全: 音乐播放中狗未出则强制创建dog_intro',
    '// Bypasses MusicDanceTick state machine which can miss the dog due to': '// 绕过MusicDanceTick状态机 (可能因Core 1优先级',
    '// prio-6 task scheduling races between dog_outro and dog_intro on Core 1.': '// 抢占导致dog_outro/dog_intro竞态漏狗)',
    "// Don't check dog_intro_done_ or dog_intro_running_ which can be corrupted by": '// 不检查dog_intro_done_/dog_intro_running_, 它们可能被',
    '// race conditions with MusicDanceTick and PerformanceTask.': '// MusicDanceTick和PerformanceTask的竞态损坏',
    '// Balance M1 motor: reverse must match forward before stopping': '// 平衡M1电机: 停前反转须匹配正转',
    '// Stop all movement': '// 停止所有运动',
    '// Send dog back and close door immediately': '// 立即收狗并关门',
    '// URL-encode the word manually for esp_http_client': '// 手动URL编码给esp_http_client',
    '// Read response body': '// 读取响应body',
    '// If already playing, stop old task cleanly to start new one.': '// 已在播放则干净停止旧任务后启动新任务',
    '// Auto-detect format from URL path': '// 从URL路径自动检测格式',
    '// URL-encode non-ASCII chars (Chinese etc.), esp_http_client does not support raw Chinese URLs': '// URL编码非ASCII字符 (中文等), esp_http_client不支持原始中文URL',
    '// Ensure path starts with /': '// 确保路径以/开头',
    '// === Time / Chime ===': '// === 时间/报时 ===',
    '// === Cantonese lookup (2026-07-19) ===': '// === 粤语查询 (2026-07-19) ===',
    '// === Music / Show ===': '// === 音乐/演出 ===',
    '// === Hardware (wiring later) ===': '// === 硬件 (后续接线) ===',
    '// === Alarms ===': '// === 闹钟 ===',
    '// === Quiet Mode ===': '// === 静音模式 ===',
    '// === Hourly Performance Toggle ===': '// === 整点演出开关 ===',
    '// NOTE(2026-07-19): threshold restore moved to the safety-net check below': '// 注意(2026-07-19): 阈值恢复移至下方安全网检查',
    '// Safety net (2026-07-19): whenever device is in quiet idle (no show,': '// 安全网(2026-07-19): 设备安静idle时 (无演出,',
    '// no music), enforce sensitive wake threshold 0.02. Fixes paths that': '// 无音乐), 强制灵敏唤醒阈值0.02, 修复',
    '// leaked 0.30: session ended during music, show finished while idle, etc.': '// 0.30泄漏: 音乐中会话结束, 演出在idle完成等路径',
    '// Flag ensures we only set once per quiet-idle entry (no log spam).': '// 标志确保每次进入安静idle只设一次 (不刷日志)',
}

parts = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']
total = 0
for p in parts:
    path = os.path.join(D, p)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    mod = False
    for i, line in enumerate(lines):
        s = line.strip()
        if s in en:
            prefix = line[:len(line)-len(s)]
            lines[i] = prefix + en[s] + '\n'
            total += 1
            mod = True
    if mod:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(lines)

print(f'Translated {total} English comments')
import subprocess
subprocess.run(['python', os.path.join(BASE, '_scan_comments.py')], check=False)

for p in parts:
    path = os.path.join(D, p)
    with open(path, 'r', encoding='utf-8') as f:
        n = len(f.readlines())
    print(f'{p}: {n} lines')
