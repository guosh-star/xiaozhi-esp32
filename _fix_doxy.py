"""Fix ALL doxygen blocks and English comments across all 5 parts."""
import os, re

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

# Map of exact doxygen block (first line + key text) -> replacement
DOXYGEN_FIXES = {
    # cuckoo_part1.cc
    ('@brief öǶ (0~180)'): '/**\n * @brief 设置角度 (0~180度)\n * @param angle 目标角度, 自动钳位 [0, 180]\n * 角度转换为50Hz PWM占空比 (409~2048对应 0~180度)\n */',
    ('@brief ƽɨ裺ʼǶȵĿǶȣָʱ'): '/**\n * @brief 平滑扫描: 从起始角度到目标角度, 指定时间\n * @param from 起始角度\n * @param to 目标角度\n * @param duration_ms 持续时间(毫秒), 每20ms一步\n */',
    ('@brief ֹͣȴ˳5ڣ'): '/**\n * @brief 停止并等待退出(最多5秒超时)\n */',
    ('@brief عPCM'): '/**\n * @brief 加载狗叫PCM\n * DecodeSingleFile/PlayUrl循环检查bark_active_标志\n * PCM叠加混音, 实现狗叫和音乐同时播放\n */',
    ('@brief 벢ŵMP3ļ'): '/**\n * @brief 解码并播放单个MP3文件\n * @param index MP3文件索引 (对应 0001~0012.mp3, 0013=小狗, 0015=琳达, 0016=花园)\n * @return 0=已停止, 1=成功\n * - ID3v2标签跳过, 使用OutputRawPcm输出\n * - 狗叫bark_active_自动叠加混音\n * - Ducking: AI说话时降到20%\n */',
    ('@brief ͨHTTPMP3ţ̨첽'): '/**\n * @brief 通过HTTP流MP3播放(后台异步)\n * @param url HTTP URL\n * - 流下载(最多4MB), 精确MPEG帧头校验, 采样率转换+重采样24000Hz\n * - Ducking: AI说话时自动降低到20%\n * - 流模式: 逐块下载逐块解码直到被打断或结束\n */',
    ('@brief ͨHTTPԭʼs16le PCM͵Ŷ'): '/**\n * @brief 通过HTTP流原始s16le PCM推送到播放队列\n * @param url PCM音频URL\n * @return 0=成功\n * - 高优先级, 4KB块PushRawPcmToPlayback推送到播放队列\n * - Ducking: AI说话时自动降低到20%\n */',
    ('@brief ͨԭʼBSD SocketOGG/OpusƵ͸'): '/**\n * @brief 通过原始BSD Socket播放OGG/Opus音频流\n * @param url Opus音频URL\n * @return 0=成功\n * - 使用BSD socket替代lwip esp_http_client, 更高效\n * - OGG容器解Opus帧 → PushPacketToDecodeQueue(原始Opus)\n * - PCM路径: legacyPushBackgroundAudio(高级音频ring buffer)\n * - Ducking: AI说话时暂停数据, 说完恢复\n * - 自动降低到65%音量(AEC回声补偿较高)\n * - 网络不通时自动切换串口模式\n */',

    # cuckoo_part2.cc
    ('@brief ringtone'): '/**\n * @brief 播放铃声\n * @param volume 音量 0.0~1.0, 开头以15%渐强到100%\n * A5(880Hz)/C#6(1100Hz)交替PCM, 每2秒切换\n */',
    ('@brief ѭ'): '/**\n * @brief 事件循环\n * FreeRTOS轮询 pending_track_ / pending_bell_hour_\n * 播放时AI静音, 结束后恢复AI\n */',
    ('@brief жϵǰǷΪڰֵ'): '/**\n * @brief 判断当前是否为黑暗环境\n * @return true=太暗, false=够亮\n */',

    # cuckoo_part3.cc
    ('@brief 򿪵ԴP-MOSFET͡5Vͨ'): '/**\n * @brief 打开电源P-MOSFET开关(5V通)\n */',
    ('@brief ʱݣ//小时分字段@param type'): '/**\n * @brief 时间段数据/小时分字段\n * @param type 类型: kPerformanceHour(整点)/kPerformanceHalf(半点)/kPerformanceManual(手动)\n * @param hour 小时(整点报时用)\n * - AI未触发5秒后自动启动\n * - 等待AI对话AbortSpeaking中断并重新初始化唤醒词\n * - Core 1 PerformanceTask执行\n */',
    ('@brief ʱš小狗+小狗Źء+赸'): '/**\n * @brief 半点报时: 小鸟+小狗+鸟门关闭+舞蹈\n * - 半点: N+0013.mp3循环(N次鸟叫+舞蹈LED+水车+舞蹈+小狗+小鸟)\n * - 整点: 3次鸟叫后开始舞蹈\n * - 结束后恢复唤醒词\n */',
    ('@brief 鲢/整点报时'): '/**\n * @brief 触发/取消整点报时\n * - 夜间(22:00-6:00)禁止报时\n * - AI正忙时背景音频无播放时间\n * - 整点(min==0): StartPerformance(kPerformanceHour)\n * - 半点(min==30): StartPerformance(kPerformanceHalf)\n */',
    ('@brief @param hour 小狗ʱ'): '/**\n * @brief 添加闹钟\n * @param hour 小时 (0-23)\n * @param minute 分钟 (0-59)\n * @param repeat_daily true=每天重复, false=一次性\n * 自动去重(相同时间只保留一个, 优先级: repeat_daily > 一次性)\n */',
    ('@brief ȡбJSON'): '/**\n * @brief 获取闹钟列表JSON\n * @return JSON字符串 [{"index":1,"hour":8,"minute":0,"enabled":true,"repeat_daily":true},...]\n */',
    ('@brief ֹͣ'): '/**\n * @brief 删除闹钟\n * 一次性闹钟触发后自动禁用, 重复闹钟保留\n */',
}

# English comments -> Chinese translations (key = exact text match)
ENGLISH_FIXES = {
    '// Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring': '// 状态机: 0=空闲, 1=淡出中, 2=已降低, 3=恢复中',
    '// Apply gain + clip': '// 应用增益 + 限幅',
    'consumed = 1;  //  Tiny files advance byte-by-byte': 'consumed = 1;  // 微小文件逐字节推进',
    '// ---- Smooth ducking: fade music out when AI starts speaking ----': '// ---- 平滑混音: AI开始说话时音乐淡出 ----',
    'if (layer != 1) return false;  // Layer 3 only': 'if (layer != 1) return false;  // 仅 Layer 3',
    '// Step 1: stereo -> mono (average L/R)': '// 第1步: 立体声 → 单声道 (L/R平均)',
    '// Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz': '// 最坏情况MP3帧缓冲(1152采样立体声) 8000→24000Hz上采样',
    '// Clear stale bg audio from previous session to prevent startup noise burst': '// 清理上回残留bg audio防止开机爆音',
    'vTaskDelay(pdMS_TO_TICKS(3000));  // Wait 3s for serial_relay to fetch data': 'vTaskDelay(pdMS_TO_TICKS(3000));  // 等3秒让串口中继取数据',
    '// HACK: Prevent audio watchdog timeout during long downloads': '// HACK: 防止长时间下载触发音频看门狗超时',
    '// Set gain based on current AI state before enabling drain': '// 开启drain前根据AI状态设置增益',
    '// Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)': '// 仅在AI真正说话时降低音量; 聆听时保持满音量(用户要求2026-07-19)',
    '// Set gain even on timeout otherwise stays at 0.001f': '// 超时也要设置增益否则停留在0.001f',
    '// Listening keeps music at 100%; duck to 50% only while AI speaks/connects': '// 聆听时音乐100%; AI说话/连接时才降到50%',
    '// When AI is speaking, pause TCP download to free WiFi airtime for': '// AI说话时暂停TCP下载释放WiFi空口时间给',
    '// UDP audio packets (prevents WiFi buffer starvation TTS stutter).': '// UDP音频包(防止WiFi缓冲区饥饿导致TTS卡顿)。',
    '// Only pause if buffer sufficient to ride through typical AI reply.': '// 仅在缓冲足够支撑AI典型回复时暂停。',
    '// Server closed connection gracefully': '// 服务器正常关闭连接',
    '// Wait 1s before retry WiFi may be reconnecting after AI conversation': '// 等1秒后重试 WiFi可能在AI对话后重连',
    '// Refresh power save in case it was changed by channel close': '// 刷新节能模式防止信道关闭时被修改',
    '// Diagnostic: log buffer fill + download rate every 8s': '// 诊断: 每8秒记录缓冲填充+下载速率',
    '// Fix(2026-07-19): idle transition during music skips threshold restore': '// 修复(2026-07-19): 音乐期间idle转换跳过阈值恢复',
    '// Decoder buffered all input; no progress possible, stop': '// 解码器缓冲了全部输入; 无法继续, 停止',
    '// Music playing: mix cuckoo sound into bg audio (no interruption)': '// 音乐播放中: 鸟叫声混入bg audio(不打断)',
    '// LED (GPIO1KS8050 B, C, 5V)': '// LED灯(GPIO1/KS8050 B, C, 5V)',
    '// Mix dog bark on top of existing bg audio (overlap, not replace)': '// 狗叫叠加在现有bg audio之上(叠加, 不替换)',
    '// Bg music may still be fading in while AI speaks: wait up to 5s for it': '// AI说话时bg music可能还在淡入: 最多等5秒',
    '// Create PerformanceTask on Core 1': '// 在Core 1创建PerformanceTask',
    '// Phase 2: music + dance (bg audio, same path as start_show/LindaShow/GardenShow)': '// Phase 2: 音乐+舞蹈(bg audio, 与start_show/LindaShow/GardenShow相同路径)',
    '// Intro (door+dog, async, ~5s) must finish before the finale': '// 开场(开门+狗, 异步, ~5秒)必须在结束前完成',
    '// Stop water wheel + LEDs off': '// 停止水车 + 关LED',
    '// Check for duplicate - update existing': '// 检查重复 - 更新已有',
    '// Shift remaining alarms down': '// 剩余闹钟下移',
    '// For one-shot alarms, disable after user stops it': '// 一次性闹钟用户停止后禁用',
}

for part in ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(d, part)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    
    # Fix doxygen blocks
    for key_text, replacement in DOXYGEN_FIXES.items():
        # Find the doxygen block containing this key text
        pattern = re.escape('/**') + r'[^*]*' + re.escape(key_text) + r'.*?\*/'
        match = re.search(pattern, content, re.DOTALL)
        if match:
            content = content.replace(match.group(), replacement)
    
    # Fix English comments
    for eng, chn in ENGLISH_FIXES.items():
        content = content.replace(eng, chn)
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part}: fixed')

print('Done')
