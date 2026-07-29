"""Fix ALL remaining doxygen blocks + translate English comments to Chinese."""
import os, re

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

# (function_name_or_unique_text, replacement_doxygen, part_filter)
DOXY_FIXES = [
    # P1 - missed blocks
    ('PlayOpus', '/**\n * @brief 通过原始BSD Socket播放OGG/Opus音频流\n * @param url Opus音频URL\n * @return 0=成功\n * - 使用BSD socket替代lwip esp_http_client, 更高效\n * - OGG容器解Opus帧 → PushPacketToDecodeQueue(原始Opus)\n * - PCM路径: legacyPushBackgroundAudio(高级音频ring buffer)\n * - Ducking: AI说话时暂停数据, 说完恢复\n * - 自动降低到65%音量(AEC回声补偿较高)\n * - 网络不通时自动切换串口模式\n */'),

    # P3 - missed blocks
    ('NeedHourlyChime', '/**\n * @brief 触发/取消整点报时\n * - 夜间(22:00-6:00)禁止报时\n * - AI正忙时背景音频无播放时间\n * - 整点(min==0): StartPerformance(kPerformanceHour)\n * - 半点(min==30): StartPerformance(kPerformanceHalf)\n */'),
    ('AddAlarm(', '/**\n * @brief 添加闹钟\n * @param hour 小时 (0-23)\n * @param minute 分钟 (0-59)\n * @param repeat_daily true=每天重复, false=一次性\n * 自动去重(相同时间只保留一个, 优先级: repeat_daily > 一次性)\n */'),
    ('GetAlarms(', '/**\n * @brief 获取闹钟列表JSON\n * @return JSON字符串 [{index:1,hour:8,minute:0,enabled:true,repeat_daily:true},...]\n */'),
    ('RemoveAlarm(', '/**\n * @brief 删除闹钟\n * 一次性闹钟触发后自动禁用, 重复闹钟保留\n */'),
    ('CheckAlarms', '/**\n * @brief 检查闹钟触发\n * - 定时匹配: 分钟==0时触发\n * - AlarmTask在后台执行\n * - 支持重复和一次性\n */'),
    ('PlayAlarmRingtone', '/**\n * @brief 闹钟播放: 循环播放50次(2秒每次, 共100秒) 15%渐强到100%, 每次之间停顿2秒\n * 期间检查alarm_stopped_标志, 用户停止则退出\n */'),
    ('DogShow(', '/**\n * @brief 小狗表演: MCP入口\n * Core 1后台创建DogShowTask()执行\n */'),

    # P4
    ('DogShowTask', '/**\n * @brief 小狗表演实现\n * 流程: AI暂停→放音乐→小狗开门→小狗前进→小狗摇尾10秒→狗叫→小狗后退→关门→恢复AI\n */'),
    ('PlayWavAsset', '/**\n * @brief 播放WAV文件\n * @param filename WAV文件名 dog_bark.wav, 2001.wav~2005.wav\n * WAV头解析后读取数据块, 通过OutputRawPcm直接播放\n * 如果是狗叫文件(鸟门打开时偏移到小狗部分)\n */'),
    ('LindaShow(', '/**\n * @brief 琳达表演: MCP入口\n */'),
    ('GardenShow(', '/**\n * @brief 花园表演: MCP入口\n */'),
    ('RunLindaShow', '/**\n * @brief 琳达表演实现\n * 流程: LED→0015.mp3播放→舞蹈+LED闪烁(前24秒)→LED全亮→结束→切换LED→恢复AI\n */'),
    ('RunGardenShow', '/**\n * @brief 花园表演实现\n * 流程: LED→0016.mp3播放→小提琴动作0~180度摆动+LED闪烁(前24秒)→LED全亮→结束→切换LED→恢复AI\n */'),
    ('start_show', '/**\n * @brief 综合表演入口: MCP cuckoo.start_show\n * Core 1后台创建 StartShowTask\n */'),
    ('StartShowTask', '/**\n * @brief 综合表演后台任务\n * AI暂停→LED+水车→选曲+小狗(异步)→舞蹈循环→LED切换→恢复AI\n */'),
    ('RunDanceIntro', '/**\n * @brief 综合表演开始: ShowTask调用的底层逻辑\n * PerformanceTask→RunDanceIntro/RunDanceLoop/RunDanceFinale\n */'),
    ('StopAll(', '/**\n * @brief 停止所有演出和音乐\n * - 停止所有M1~M4 + 小提琴 + 水车\n * - 舵机归位90度\n * - LED熄灭\n * - 音乐停止(不是静音模式)\n * - 通过Schedule异步将设备状态设为Idle\n */'),
    ('StopMusic(', '/**\n * @brief 停止音乐, 音频通道\n */'),
    ('OpenDoor(', '/**\n * @brief 鸟门打开: M2正转 MAIN_DOOR_TIME_MS\n */'),
    ('CloseDoor(', '/**\n * @brief 鸟门关闭: M2反转 MAIN_DOOR_TIME_MS\n */'),
    ('BirdJumpPulse', '/**\n * @brief 小鸟弹跳: 通电500ms + 等待400ms\n */'),
    ('WaterBirdJump(', '/**\n * @brief 小鸟弹跳(AI说话时触发)\n * 随机50-200ms通电, 等待300-700ms\n */'),

    # P5
    ('set_servo', '/**\n * @brief 设置舵机角度\n * @param servo_id 0=小提琴舵机, 1=狗尾巴舵机\n * @param angle 0~180度\n */'),
    ('set_motor', '/**\n * @brief 设置电机速度\n * @param motor_id 1=M1舞蹈电机, 2=小提琴电机, 3=小狗, 4=鸟门电机\n * @param speed -100~100 (正=正转)\n */'),
    ('play_url', '/**\n * @brief 播放URL音乐\n * @param url_or_path URL或路径 \"/pcm?q=周杰伦\"\n * @return 0=成功, <0=失败\n * - AI对话中不占用CPU\n * - 自动URL编码中文和空格\n * - 缺少http前缀自动拼接代理地址\n * - 已在播放时停止旧歌再发新歌\n */'),
    ('set_music_proxy', '/**\n * @brief 设置音乐代理地址\n * @param host IP或域名\n * @param port 端口\n */'),
]

# English -> Chinese translations
ENG_FIXES = {
    # P3
    ' // Stop any playing audio': ' // 停止所有正在播放的音频',
    ' // Play alarm ringtone x50 (2s each = 100s total ringing)': ' // 播放闹钟铃声 x50次 (每次2秒 = 共100秒)',
    ' // First round: ramp volume 15%100% over first 10 calls (20s)': ' // 首轮: 前10次(20秒)音量从15%渐强到100%',
    ' // Snooze: wait 2 minutes, checking stopped_ every second': ' // 贪睡: 等待2分钟, 每秒检查stopped_标志',

    # P4
    '    // Find data chunk (may have fmt, LIST etc before it)': '    // 查找data块 (前面可能有fmt/LIST等块)',
    '        // DogShow path: no bg audio, use OutputRawPcm directly': '        // DogShow路径: 无bg audio, 直接使用OutputRawPcm',
    '    // Push done: wait for ring buffer to drain, then clear bg audio flags': '    // 推送完成: 等待ring buffer排空, 然后清除bg audio标志',
    '    // Wait for bg audio to start draining': '    // 等待bg audio开始排出',
    '    // Use PlayShowMusicBg (bg audio ring buffer) like LindaShow/GardenShow': '    // 使用PlayShowMusicBg(bg audio ring buffer) 与LindaShow/GardenShow一致',
    '    // Clean up bg audio AFTER finale completes (previously it was before, causing state gap)': '    // final结束后再清理bg audio(之前放在前面导致状态空窗)',
    '    // Fix(2026-07-19): show runs while device stays idle, no state transition,': '    // 修复(2026-07-19): 演出在idle状态下运行, 无状态转换,',

    # P5
    '// Forward declarations for types/functions defined in P1-P2': '// P1-P2中定义的类型/函数的前置声明',
    '// RTC crash log struct (defined in P1, replicated here for independent compilation)': '// RTC崩溃日志结构体(在P1中定义, 此处为独立编译而复制)',
    '    // Mp3Player tracks playback state (stays true during AI speech ducking),': '    // Mp3Player跟踪播放状态(AI语音混音期间保持为true),',
    '    // Using both ensures music dance survives AI conversations.': '    // 两者结合确保音乐舞蹈能在AI对话中持续。',
    '    // Track kids_active_ transition for mid-music changes': '    // 跟踪kids_active_转换以便处理音乐中途变化',
    '        // MusicDanceTick log muted to reduce noise (prints every 270ms)': '        // MusicDanceTick日志静默以减少噪音(每270ms打印一次)',
    '        // Dog comes out when music plays &amp; kids active. NOT gated on': '        // 音乐播放且kids激活时狗出来。不依赖',
    '        // Wait for dog_intro to finish before taking over servo/motor': '        // 等待dog_intro完成再接管舵机/电机',
    '            // Dance motor: Phase 0-3 fwd 20ms, Phase 4-7 rev 20/21ms': '            // 舞蹈电机: Phase 0-3正转20ms, Phase 4-7反转20/21ms',
    '            // Guitar + Dog servos: sync to same phase rhythm': '            // 小提琴 + 小狗舵机: 同步相同相位节奏',
    '            // Phase 0-3: forward beat, Phase 4-7: backward beat': '            // Phase 0-3: 向前节拍, Phase 4-7: 向后节拍',
    '        // Don\'t shut down during an active performance (ShowTask/KidsDanceShow)': '        // 有活跃演出时不关闭(ShowTask/KidsDanceShow)',
    '        // MusicDT-ELSE log muted to reduce noise (prints every 250ms)': '        // MusicDT-ELSE日志静默以减少噪音(每250ms打印一次)',
    '            // Balance M1 motor: reverse must match forward': '            // 平衡M1电机: 反转必须匹配正转',
    '            // Music ended: dog goes back, close door (only if kids were active)': '            // 音乐结束: 狗回去, 关门(仅在kids激活时)',
    '        // Turn off LEDs when music ends': '        // 音乐结束时关闭LED',
    '    // Re-enable motor power in case MusicDogOutro turned it off mid-way': '    // 重新开启电机电源以防MusicDogOutro中途将其关闭',
    '    // First smooth return to 30deg, then back to home': '    // 先平滑回到30度, 再归位',
    '    // Set kids_active_ NOW so MusicDanceTick doesn\'t enter else branch': '    // 立即设置kids_active_防止MusicDanceTick进入else分支',
    '        // Visible dance: same big moves as hourly chime': '        // 可见舞蹈: 与整点报时相同的大动作',
    '            // Music ended: full cleanup': '            // 音乐结束: 完整清理',
    '    // Safety: if music is playing and dog isn\'t out yet, force-create dog_intro.': '    // 安全保护: 如果音乐在播放且狗还没出来, 强制创建dog_intro。',
    '    // Bypasses MusicDanceTick state machine which can miss the dog due to': '    // 绕过MusicDanceTick状态机(可能因竞态条件错过狗的相关逻辑)',
    '    // Don\'t check dog_intro_done_ or dog_intro_running_ which can be corrupted by': '    // 不检查dog_intro_done_或dog_intro_running_(可能被竞态条件破坏)',
    '    // Balance M1 motor: reverse must match forward before stopping': '    // 平衡M1电机: 停止前反转必须匹配正转',
    '    // Stop all movement': '    // 停止所有运动',
    '    // Send dog back and close door immediately': '    // 立即送狗回去并关门',
    '    // URL-encode the word manually for esp_http_client': '    // 手动URL编码搜索词供esp_http_client使用',
    '    // Read response body': '    // 读取响应体',
    '    // If already playing, stop old task cleanly to start new one.': '    // 如果已经在播放, 先干净停止旧任务再开始新任务。',
    '    // Auto-detect format from URL path': '    // 从URL路径自动检测格式',
    ' // URL-encode non-ASCII chars (Chinese etc.), esp_http_client does not support raw Chinese URLs': ' // URL编码非ASCII字符(中文等), esp_http_client不支持原始中文URL',
    ' // Ensure path starts with /': ' // 确保路径以/开头',
    '                // NOTE(2026-07-19): threshold restore moved to the safety-net check below': '                // 注意(2026-07-19): 阈值恢复已移至下面的安全网检查',
    '            // Safety net (2026-07-19): whenever device is in quiet idle (no show,': '            // 安全网(2026-07-19): 当设备处于安静空闲时(无演出,',
    '            // Flag ensures we only set once per quiet-idle entry (no log spam).': '            // 标志确保每次安静空闲进入只设置一次(不刷屏)。',
}

for part in ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(d, part)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    changes = 0
    
    # Fix doxygen blocks
    for func_name, replacement in DOXY_FIXES:
        pattern = r'(/\*\*.*?\*/)\s*\n[^\n]*' + re.escape(func_name)
        match = re.search(pattern, content, re.DOTALL)
        if match:
            old_block = match.group(1)
            if '/**' in old_block and '*/' in old_block:
                content = content.replace(old_block, replacement)
                changes += 1
    
    # Fix English comments
    for eng, chn in ENG_FIXES.items():
        if eng in content:
            content = content.replace(eng, chn)
            changes += 1
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part}: {changes} fixes')
    else:
        print(f'{part}: no changes')

print('Done!')
