"""Replace garbled doxygen blocks using function name as anchor."""
import os, re

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

# (function_name_after_block, replacement_doxygen)
doxy_map = [
    ('SetAngle', '/**\n * @brief 设置角度 (0~180度)\n * @param angle 目标角度, 自动钳位 [0, 180]\n * 角度转换为50Hz PWM占空比 (409~2048对应 0~180度)\n */'),
    ('Sweep(', '/**\n * @brief 平滑扫描: 从起始角度到目标角度, 指定时间\n * @param from 起始角度\n * @param to 目标角度\n * @param duration_ms 持续时间(毫秒), 每20ms一步\n */'),
    ('~Mp3Player', '/**\n * @brief 停止并等待退出(最多5秒超时)\n */'),
    ('LoadDogBark', '/**\n * @brief 加载狗叫PCM\n * DecodeSingleFile/PlayUrl循环检查bark_active_标志\n * PCM叠加混音, 实现狗叫和音乐同时播放\n */'),
    ('DecodeSingleFile', '/**\n * @brief 解码并播放单个MP3文件\n * @param index MP3文件索引 (对应 0001~0012.mp3, 0013=小狗, 0015=琳达, 0016=花园)\n * @return 0=已停止, 1=成功\n * - ID3v2标签跳过, 使用OutputRawPcm输出\n * - 狗叫bark_active_自动叠加混音\n * - Ducking: AI说话时降到20%\n */'),
    ('PlayUrl(char', '/**\n * @brief 通过HTTP流MP3播放(后台异步)\n * @param url HTTP URL\n * - 流下载(最多4MB), 精确MPEG帧头校验, 采样率转换+重采样24000Hz\n * - Ducking: AI说话时自动降低到20%\n * - 流模式: 逐块下载逐块解码直到被打断或结束\n */'),
    ('PlayPcm(', '/**\n * @brief 通过HTTP流原始s16le PCM推送到播放队列\n * @param url PCM音频URL\n * @return 0=成功\n * - 高优先级, 4KB块PushRawPcmToPlayback推送到播放队列\n * - Ducking: AI说话时自动降低到20%\n */'),
    ('PlayOpus(char', '/**\n * @brief 通过原始BSD Socket播放OGG/Opus音频流\n * @param url Opus音频URL\n * @return 0=成功\n * - 使用BSD socket替代lwip esp_http_client, 更高效\n * - OGG容器解Opus帧 -> PushPacketToDecodeQueue(原始Opus)\n * - PCM路径: legacyPushBackgroundAudio(高级音频ring buffer)\n * - Ducking: AI说话时暂停数据, 说完恢复\n * - 自动降低到65%音量(AEC回声补偿较高)\n * - 网络不通时自动切换串口模式\n */'),
    ('BellSoundPlayer::Play(', '/**\n * @brief 播放铃声\n * @param volume 音量 0.0~1.0, 开头以15%渐强到100%\n * A5(880Hz)/C#6(1100Hz)交替PCM, 每2秒切换\n */'),
    ('CuckooStateMachine::EventLoop', '/**\n * @brief 事件循环\n * FreeRTOS轮询 pending_track_ / pending_bell_hour_\n * 播放时AI静音, 结束后恢复AI\n */'),
    ('IsDark', '/**\n * @brief 判断当前是否为黑暗环境\n * @return true=太暗, false=够亮\n */'),
    ('MotorPowerOn', '/**\n * @brief 打开电源P-MOSFET开关(5V通)\n */'),
    ('StartPerformance(', '/**\n * @brief 启动定时表演\n * @param type 类型: kPerformanceHour(整点)/kPerformanceHalf(半点)/kPerformanceManual(手动)\n * @param hour 小时(整点报时用)\n * - AI未触发5秒后自动启动\n * - 等待AI对话AbortSpeaking中断并重新初始化唤醒词\n * - Core 1 PerformanceTask执行\n */'),
    ('PerformanceTask(', '/**\n * @brief 半点报时: 小鸟+小狗+鸟门关闭+舞蹈\n * - 半点: N+0013.mp3循环(N次鸟叫+舞蹈LED+水车+舞蹈+小狗+小鸟)\n * - 整点: 3次鸟叫后开始舞蹈\n * - 结束后恢复唤醒词\n */'),
    ('NeedHourlyChime', '/**\n * @brief 触发/取消整点报时\n * - 夜间(22:00-6:00)禁止报时\n * - AI正忙时背景音频无播放时间\n * - 整点(min==0): StartPerformance(kPerformanceHour)\n * - 半点(min==30): StartPerformance(kPerformanceHalf)\n */'),
    ('AddAlarm(', '/**\n * @brief 添加闹钟\n * @param hour 小时 (0-23)\n * @param minute 分钟 (0-59)\n * @param repeat_daily true=每天重复, false=一次性\n * 自动去重(相同时间只保留一个, 优先级: repeat_daily > 一次性)\n */'),
    ('GetAlarms(', '/**\n * @brief 获取闹钟列表JSON\n * @return JSON字符串 [{index:1,hour:8,minute:0,enabled:true,repeat_daily:true},...]\n */'),
    ('RemoveAlarm(', '/**\n * @brief 删除闹钟\n * 一次性闹钟触发后自动禁用, 重复闹钟保留\n */'),
]

for part in ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']:
    path = os.path.join(d, part)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    changed = 0
    for func_name, replacement in doxy_map:
        pattern = r'(/\*\*.*?\*/)\s*\n[^\n]*' + re.escape(func_name)
        match = re.search(pattern, content, re.DOTALL)
        if match:
            old_block = match.group(1)
            if '/**' in old_block and '*/' in old_block:
                content = content.replace(old_block, replacement)
                changed += 1
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part}: {changed} doxygen blocks fixed')
    else:
        print(f'{part}: no changes')
