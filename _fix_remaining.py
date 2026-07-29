"""
ADDITIONAL mappings for _fix_by_line.py. Append these to the script.
Covers remaining garbled lines from _garb_comments.txt.
"""
import os

# Additional fixes for all remaining garbled lines
MORE_FIXES = {
    'cuckoo_part1.cc': {
        # Remaining P1 garbled lines
        70: "// \"/stream\" = 7个字符, \"/pcm\" = 4个字符, 向后移 \"?...\" 到 pos+7 处",
        80: "// \"/opus\" = 6个字符, \"/pcm\" = 4个字符, 向后移 \"?...\" 到 pos+5 处",
        87: "// 也可以处理不带前斜杠的路径（当AI省略时： \"stream?q=...\" 或 \"opus?q=...\"）",
        90: "// \"stream\" = 6个字符, \"pcm\" = 3个字符, 向后移 \"?...\" 到 pos+6 处",
        99: "// \"opus\" = 4个字符, \"pcm\" = 3个字符, 向后移 \"?...\" 到 pos+4 处",
        113: "// 舵机: 2路 PWM 通道 (频率 50Hz)",
        114: "// 水泵电机 L9110S: 1路 PWM 通道 (频率 ~1-10KHz)",
        115: "// 共7路通道",
        118: "// 电机驱动 (TB6612 / DRV8833)",
        121: "// 通用GPIO初始化，支持直驱动PWM双模式",
        128: "// GPIO模式构造函数——不配置PWM",
        131: "// PWM模式构造函数，小提琴舵机 (timer0, 1kHz/10-bit, 跟dog-test一致)",
        146: "// PWM模式构造函数，自定义定时器和电机timer，如指定",
        184: "// 舵机 (SG90)",
        189: "// 定时器全局只初始化一次（50Hz, 14-bit分辨率）",
        219: "* @brief 设定舵机角度 (0~180度)",
        220: "* @param angle 目标角度，自动钳位到 [0, 180]",
        221: "* 将角度转为50Hz PWM占空比 (409~2048对应 0~180度)",
        233: "* @brief 平滑扫描：从起始角度到目标角度，在指定时间内完成",
        234: "* @param from 起始角度",
        235: "* @param to 目标角度",
        236: "* @param duration_ms 完成时间（毫秒），每20ms一步",
        249: "// 小鸟跳跃 + 门铃水车驱动 (DRV8833 电机驱动 IN1/IN2)",
        250: "// config.h 定义: MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP",
        251: "// 水车正转: water_bird_->SetSpeed(WATER_WHEEL_SPEED) — IN1导通",
        252: "// 水车反转: water_bird_->SetSpeed(-100) — IN2导通",
        255: "// MP3播放器 (后台解码与异步播放)",
        257: "// Mp3Player - 音乐解码播放（异步后台栈）",
        258: "// 支持: DecodeSingleFile(离线MP3) / PlayUrl(HTTP流MP3) / PlayPcm(原始PCM) / PlayOpus(OGG/Opus)",
        259: "// Ducking: AI说话时自动把音乐从100%降至20%，说完了渐恢复至100%",
        260: "// 声音叠加: LoadDogBark加载PCM狗叫至叠加层，可在解码循环中调用",
        264: "* @brief 析构函数，停止并等待线程退出（5秒超期，超时则强制终止）",
        268: "// 退出超期5秒则强制退，不recv直接关socket",
        288: "// 功能: Ducking降音 与 狗叫声叠加（背景音频混音）",
        290: "// 功能: Ducking降音 与 狗叫声叠加（背景音频混音）",
        314: "* @brief 加载狗叫PCM数据至叠加缓冲区",
        315: "* 在DecodeSingleFile/PlayUrl的解码循环中检查bark_active_",
        316: "* 将狗叫PCM叠加到输出缓冲上，实现狗叫和音乐同时播放",
        341: "* @brief 解码并播放单个离线MP3文件",
        342: "* @param index MP3文件编号 (对应 0001~0012.mp3、0013为琳达、0015为琳达、0016为园子)",
        343: "* @return 0=正常停止, 1=持续播放",
        344: "* - 跳过ID3v2标签，Minimp3解码，OutputRawPcm输出",
        345: "* - 循环检查bark_active_自动叠加狗叫声",
        346: "* - Ducking：AI说话时降至20%音量",
        357: "// 从 assets 读取 MP3 文件数据",
        372: "// 跳过 ID3v2 标签：MP3 文件以 \"ID3\" 开头时，前10字节后有一个 sync-safe int 是标签长度）：",
        386: "// 关闭解码器",
        398: "// 解码信息",
        404: "// 解码循环",
        411: "// 准备输入帧",
        421: "// 准备输出帧",
        428: "// 解码",
        432: "// 首次获取解码信息 — esp_mp3_dec_decode 会通过 dec_info 填出",
        441: "// 有解码出的 PCM 数据，推给音频通道",
        455: "// 降音模式：智能音量管理",
        458: "// 开始降音",
        465: "// 400ms降至20%",
        473: "// AI在降音期间停止了 — 恢复",
        484: "// 400ms恢复到100%",
        493: "// AI在恢复期间又开始了说话 — 重新降音",
        508: "// 在输出缓冲中叠加狗叫数据（做增益处理等）",
        529: "// 等待播放完成，OutputRawPcm 内部阻塞",
        535: "// 移动解码指针",
        552: "// 继续拉取并解码下一帧",
        566: "// 关闭解码器",
        572: "// 恢复小概率的 Opus 解码器错误（如果解码器没容错则恢复之）",
        584: "//   3. 解析 JSON 响应 (提取 url 字段并重定向)",
        592: "// 后台流播放实现",
        604: "* @brief 通过HTTP流下载MP3并播放（后台异步任务）",
        606: "* - 流下载：最多4MB缓冲，严格MPEG帧头校验，智能重连 + 重采样至24000Hz",
        607: "* - Ducking：AI说话时自动降至20%",
        608: "* - 格式混淆：消费一半缓冲后即循环播放，直到被中断或被打断",
        610: "// 严格 MPEG 帧头校验——不只看 FF Ex，还校验 bitrate/samplerate/layer",
        635: "// HTTP 客户端连接",
        660: "// 获取长度",
        672: "// 循环下载：如果Content-Length已知，缓冲满即停；否则（分块传输）边拉边消费",
        681: "// PSRAM分配失败——降到256KB极限",
        696: "// 重采样缓冲区（用于非16k源 + 立体声转单声道）",
        699: "// 最坏帧size = 1152, 重采样最大因子 = 24000/8000=3.0, 最大样本数 = 3456",
        700: "// 取4096保证安全余量并方便合并操作",
        715: "// 第一次连接 + 预缓冲：跳过ID3标签 + 向后搜索采样率",
        729: "// ---- 跳过 ID3v2 标签 ----",
        739: "// ---- 从原始数据扫描 MPEG 帧头提取采样率 ----",
        754: "// ---- 解码循环：消费缓冲并播放 ----",
        800: "// 解码边消费边播放 + 重采样到24000Hz",
        801: "// OutputRawPcm内部重采样到24000Hz，如果不重采样会导致音调错误；",
        808: "// 第2步：线性插值重采样到kOutRate",
        820: "// 自适应响应 Ducking 降音",
        834: "// 自适应响应 Ducking 降音",
        846: "// 用解码器返回的 frame_size 而非 raw.consumed 确定帧边界更精确",
        854: "// 当帧起始位置未发现帧头时，向后搜下一个 MPEG 同步帧并丢弃乱序数据",
        866: "// 读取失败或未读完——重新切片从下一个 MPEG 帧头开始",
        877: "// ----- 把缓冲区末尾未消费字节，拼接到下一轮开头 -----",
        885: "// ----- 下载下一批 -----",
        898: "// carry=0且无帧头则扫描metadata/非帧数据直到下一严格帧头",
        929: "// PlayPcm: HTTP下载原始PCM — PushRawPcmToPlayback（旧式播放管线，无帧边界问题）",
        931: "* @brief 通过HTTP下载原始s16le PCM并送到播放管线",
        932: "* @param url PCM音频URL",
        933: "* @return 0=播放成功",
        934: "* - 分块下载：4KB块PushRawPcmToPlayback送播管线",
        935: "* - Ducking：AI说话时自动降至20%",
        1001: "// 分块下载PCM并送播管线",
        1033: "// 自适应响应 Ducking 降音",
        1044: "// 每批量约4个音频帧后交出CPU，防止WiFi拥挤",
        1063: "* @brief 通过原始BSD Socket下载OGG/Opus音频并送到音频管线",
        1064: "* @param url Opus音频URL",
        1065: "* @return 0=播放成功",
        1066: "* - 使用BSD socket直接规避lwip esp_http_client",
        1067: "* - Opus路径：OGG解封装 → PushPacketToDecodeQueue（复用Opus解码管线）",
        1068: "* - PCM路径（legacy）：PushBackgroundAudio（高级背景音频 ring buffer）",
        1069: "* - Ducking：AI说话时暂停下载数据，等说完了恢复下载",
        1070: "* - 自动音量降至65%（减轻AEC回采干扰）",
        1071: "* - 网络不通时优雅降级为串口模式",
    },
    'cuckoo_part2.cc': {
        # P2 header (offset 50 was correct - lines 52-70 already fixed in first pass)
        # Remaining P2 lines
        2: "// 布谷鸟钟控制器 (Cuckoo Controller)",  # Lines without offset
        4: "// 功能概述",
        5: "// - 电机控制（4路直流电机 + 1路舵机 + 水车控制）",
        6: "// - 舵机控制：小提琴舵机、小狗尾巴舵机",
        7: "// - LED 闪烁：多路 LED 呼吸/闪烁",
        8: "// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载；Ducking 智能混合",
        9: "// - 整点报时：开门报时 + 小鸟跳跃 + 音乐演奏 + 报时",
        10: "// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 灯光 + LED",
        11: "// - 特色表演 MCP 工具：DogShow（小狗出）、LindaShow（琳达）、GardenShow（园子）",
        12: "// - 闹钟功能：NVS 持久化、最多5个闹钟、支持每日重复",
        13: "// - 静音时段：夜间模式 22:00-6:00 静音",
        14: "// - 在线音乐不让duck播放：QQ音乐代理下载 Opus/PCM 格式",
        15: "// - MCP 工具注册（21个工具，通过 MCP 协议供 AI 大模型调用）",
        17: "// 线程架构",
        18: "// - Core 0: 语音处理（唤醒词、TTS、Opus解码）",
        19: "// - Core 1: 钟控（cuckoo_clock_task，250ms tick，处理时间与报时）",
        20: "// - 音乐/演出任务通过 xTaskCreatePinnedToCore 在 Core 1 执行",
        120: "// 原始BSD socket",
        512: "// 原来 while 等待排空会导致 MusicDanceTick 继续跑约5秒（79K样本÷16kHz）",
    },
    'cuckoo_part3.cc': {
        # P3 header (offset was wrong - header is at lines 2-20)
        2: "// 布谷鸟钟控制器 (Cuckoo Controller)",
        4: "// 功能概述",
        5: "// - 电机控制（4路直流电机 + 1路舵机 + 水车控制）",
        6: "// - 舵机控制：小提琴舵机、小狗尾巴舵机",
        7: "// - LED 闪烁：多路 LED 呼吸/闪烁",
        8: "// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载；Ducking 智能混合",
        9: "// - 整点报时：开门报时 + 小鸟跳跃 + 音乐演奏 + 报时",
        10: "// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 灯光 + LED",
        11: "// - 特色表演 MCP 工具：DogShow（小狗出）、LindaShow（琳达）、GardenShow（园子）",
        12: "// - 闹钟功能：NVS 持久化、最多5个闹钟、支持每日重复",
        13: "// - 静音时段：夜间模式 22:00-6:00 静音",
        14: "// - 在线音乐不让duck播放：QQ音乐代理下载 Opus/PCM 格式",
        15: "// - MCP 工具注册（21个工具，通过 MCP 协议供 AI 大模型调用）",
        17: "// 线程架构",
        18: "// - Core 0: 语音处理（唤醒词、TTS、Opus解码）",
        19: "// - Core 1: 钟控（cuckoo_clock_task，250ms tick，处理时间与报时）",
        20: "// - 音乐/演出任务通过 xTaskCreatePinnedToCore 在 Core 1 执行",
        784: "* @param hour 小时 (0-23)",
        787: "* 自动去重（同时间则合并到repeat_daily）",
        817: "* @return JSON格式字符串，如[{\"index\":1,\"hour\":8,\"minute\":0,\"enabled\":true,\"repeat_daily\":true},...]",
        912: "* 期间检查alarm_stopped_标志，若用户停止则立即退出",
    },
    'cuckoo_part4.cc': {
        # P4 header
        2: "// 布谷鸟钟控制器 (Cuckoo Controller)",
        4: "// 功能概述",
        5: "// - 电机控制（4路直流电机 + 1路舵机 + 水车控制）",
        6: "// - 舵机控制：小提琴舵机、小狗尾巴舵机",
        7: "// - LED 闪烁：多路 LED 呼吸/闪烁",
        8: "// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载；Ducking 智能混合",
        9: "// - 整点报时：开门报时 + 小鸟跳跃 + 音乐演奏 + 报时",
        10: "// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 灯光 + LED",
        11: "// - 特色表演 MCP 工具：DogShow（小狗出）、LindaShow（琳达）、GardenShow（园子）",
        12: "// - 闹钟功能：NVS 持久化、最多5个闹钟、支持每日重复",
        13: "// - 静音时段：夜间模式 22:00-6:00 静音",
        14: "// - 在线音乐不让duck播放：QQ音乐代理下载 Opus/PCM 格式",
        15: "// - MCP 工具注册（21个工具，通过 MCP 协议供 AI 大模型调用）",
        17: "// 线程架构",
        18: "// - Core 0: 语音处理（唤醒词、TTS、Opus解码）",
        19: "// - Core 1: 钟控（cuckoo_clock_task，250ms tick，处理时间与报时）",
        20: "// - 音乐/演出任务通过 xTaskCreatePinnedToCore 在 Core 1 执行",
        187: "* @param filename WAV文件名，如 dog_bark.wav, 2001.wav~2005.wav",
        188: "* 通过WAV头获取采样率和数据块，通过OutputRawPcm直接播放",
        189: "* 非狗叫文件做音量放大处理（补偿音量偏小问题）",
        349: "* 流程：LED亮→播放0015.mp3→舞蹈电机动→LED交替闪烁（前24秒）→LED全亮→音乐结束后→亮感谢灯→LED熄灭→恢复",
        380: "//    前24秒：LED交替闪烁 + 舞蹈",
        381: "//    24秒后：LED全亮，舞蹈持续直到音乐结束",
        511: "* 流程：LED亮→播放0016.mp3→小提琴动0~180度摆动+LED交替闪烁（前24秒）→LED全亮→音乐结束后→归位→亮感谢灯→LED熄灭→恢复",
        758: "* 与PerformanceTask共用RunDanceIntro/RunDanceLoop/RunDanceFinale",
    },
    'cuckoo_part5.cc': {
        # P5 header
        2: "// 布谷鸟钟控制器 (Cuckoo Controller)",
        4: "// 功能概述",
        5: "// - 电机控制（4路直流电机 + 1路舵机 + 水车控制）",
        6: "// - 舵机控制：小提琴舵机、小狗尾巴舵机",
        7: "// - LED 闪烁：多路 LED 呼吸/闪烁",
        8: "// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载；Ducking 智能混合",
        9: "// - 整点报时：开门报时 + 小鸟跳跃 + 音乐演奏 + 报时",
        10: "// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 灯光 + LED",
        11: "// - 特色表演 MCP 工具：DogShow（小狗出）、LindaShow（琳达）、GardenShow（园子）",
        12: "// - 闹钟功能：NVS 持久化、最多5个闹钟、支持每日重复",
        13: "// - 静音时段：夜间模式 22:00-6:00 静音",
        14: "// - 在线音乐不让duck播放：QQ音乐代理下载 Opus/PCM 格式",
        15: "// - MCP 工具注册（21个工具，通过 MCP 协议供 AI 大模型调用）",
        17: "// 线程架构",
        18: "// - Core 0: 语音处理（唤醒词、TTS、Opus解码）",
        19: "// - Core 1: 钟控（cuckoo_clock_task，250ms tick，处理时间与报时）",
        20: "// - 音乐/演出任务通过 xTaskCreatePinnedToCore 在 Core 1 执行",
        419: "* @param url_or_path URL或歌曲路径，如 \"/pcm?q=周杰伦\"",
        420: "* @return 0=成功, <0=失败",
    },
}

# Apply these fixes
BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
D = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')

total = 0
for part_name, fixes in MORE_FIXES.items():
    path = os.path.join(D, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    for ln in sorted(fixes.keys(), reverse=True):
        idx = ln - 1
        if idx < 0 or idx >= len(lines):
            print(f'  SKIP: {part_name} L{ln} out of range')
            continue
        orig = lines[idx].rstrip('\n\r')
        if '\ufffd' in orig:
            indent = orig[:len(orig)-len(orig.lstrip())]
            lines[idx] = indent + fixes[ln] + '\n'
            total += 1
    
    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(lines)

print(f'Applied {total} additional garbled fixes')

# Re-scan
os.system(f'python {os.path.join(BASE, "_scan_comments.py")}')

# Verify line counts
for p in ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']:
    path = os.path.join(D, p)
    with open(path, 'r', encoding='utf-8') as f:
        n = len(f.readlines())
    print(f'{p}: {n} lines')
