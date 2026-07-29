"""Fix ALL garbled + English comments in 5 part files.
Uses exact-line matching. Safe: only replaces comment lines, preserves code."""
import os, sys
sys.stdout.reconfigure(encoding='utf-8')

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
D = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
PARTS = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

# ===== GARBLED → CORRECT CHINESE MAPPING =====
# Key: garbled line (with whitespace stripped but comment prefix preserved)
# Value: correct Chinese line

GARB_MAP = {
    # ---- FILE HEADER (in all 5 parts) ----
    "// 布谷鸟钟控制器 (Cuckoo Controller)": "// 布谷鸟钟控制器 (Cuckoo Controller)",
    "// 功能概述": "// 功能概述",
    "// - 电机控制（4路直流电机 + 1路舵机 + 水车控制）": "// - 电机控制（4路直流电机 + 1路舵机 + 水车控制）",
    "// - 舵机控制：小提琴舵机、小狗尾巴舵机": "// - 舵机控制：小提琴舵机、小狗尾巴舵机",
    "// - LED 闪烁：多路 LED 呼吸/闪烁": "// - LED 闪烁：多路 LED 呼吸/闪烁",
    "// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载；Ducking 智能混合": "// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载；Ducking 智能混合",
    "// - 整点报时：开门报时 + 小鸟跳跃 + 音乐演奏 + 报时": "// - 整点报时：开门报时 + 小鸟跳跃 + 音乐演奏 + 报时",
    "// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 灯光 + LED": "// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 灯光 + LED",
    "// - 特色表演 MCP 工具：DogShow（小狗出）、LindaShow（琳达）、GardenShow（园子）": "// - 特色表演 MCP 工具：DogShow（小狗出）、LindaShow（琳达）、GardenShow（园子）",
    "// - 闹钟功能：NVS 持久化、最多5个闹钟、支持每日重复": "// - 闹钟功能：NVS 持久化、最多5个闹钟、支持每日重复",
    "// - 静音时段：夜间模式 22:00-6:00 静音": "// - 静音时段：夜间模式 22:00-6:00 静音",
    "// - 在线音乐不让duck播放：QQ音乐代理下载 Opus/PCM 格式": "// - 在线音乐不让duck播放：QQ音乐代理下载 Opus/PCM 格式",
    "// - MCP 工具注册（21个工具，通过 MCP 协议供 AI 大模型调用）": "// - MCP 工具注册（21个工具，通过 MCP 协议供 AI 大模型调用）",
    "// 线程架构": "// 线程架构",
    "// - Core 0: 语音处理（唤醒词、TTS、Opus解码）": "// - Core 0: 语音处理（唤醒词、TTS、Opus解码）",
    "// - Core 1: 钟控（cuckoo_clock_task，250ms tick，处理时间与报时）": "// - Core 1: 钟控（cuckoo_clock_task，250ms tick，处理时间与报时）",
    "// - 音乐/演出任务通过 xTaskCreatePinnedToCore 在 Core 1 执行": "// - 音乐/演出任务通过 xTaskCreatePinnedToCore 在 Core 1 执行",
    
    # ---- P1 specific ----
    "// RTC 崩溃日志存：崩溃/复位时记录状，下次开机自动导出": "// RTC 崩溃日志存储：崩溃/复位时记录状态，下次开机自动导出",
    "// 将服务器返回的 /stream 或 /opus 格式的URL转为 /pcm 格式供ESP32解码音频使用": "// 将服务器返回的 /stream 或 /opus 格式的URL转为 /pcm 格式供ESP32解码音频使用",
    "// 直接替换字符串中对应部分": "// 直接替换字符串中对应部分",
    "\"/stream\" = 7个字符, \"/pcm\" = 4个字符, 向后移 \"?...\" 到 pos+7 处": "\"/stream\" = 7个字符, \"/pcm\" = 4个字符, 向后移 \"?...\" 到 pos+7 处",
    "\"/opus\" = 6个字符, \"/pcm\" = 4个字符, 向后移 \"?...\" 到 pos+5 处": "\"/opus\" = 6个字符, \"/pcm\" = 4个字符, 向后移 \"?...\" 到 pos+5 处",
    "也可以处理不带前斜杠的路径（当AI省略时： \"stream?q=...\" 或 \"opus?q=...\"）": "也可以处理不带前斜杠的路径（当AI省略时： \"stream?q=...\" 或 \"opus?q=...\"）",
    "\"stream\" = 6个字符, \"pcm\" = 3个字符, 向后移 \"?...\" 到 pos+6 处": "\"stream\" = 6个字符, \"pcm\" = 3个字符, 向后移 \"?...\" 到 pos+6 处",
    "\"opus\" = 4个字符, \"pcm\" = 3个字符, 向后移 \"?...\" 到 pos+4 处": "\"opus\" = 4个字符, \"pcm\" = 3个字符, 向后移 \"?...\" 到 pos+4 处",
    "// LEDC PWM 通道分配": "// LEDC PWM 通道分配",
    "TB6612 电机驱动: 4路 PWM 通道 (频率 ~10-100KHz)": "TB6612 电机驱动: 4路 PWM 通道 (频率 ~10-100KHz)",
    "舵机: 2路 PWM 通道 (频率 50Hz)": "舵机: 2路 PWM 通道 (频率 50Hz)",
    "水泵电机 L9110S: 1路 PWM 通道 (频率 ~1-10KHz)": "水泵电机 L9110S: 1路 PWM 通道 (频率 ~1-10KHz)",
    "共7路通道": "共7路通道",
    "// 电机驱动 (TB6612 / DRV8833)": "// 电机驱动 (TB6612 / DRV8833)",
    "// GPIO初始，支持直驱动PWM双模式": "// GPIO初始化，支持直驱动PWM双模式",
    "// GPIO模式构函数--不配置PWM": "// GPIO模式构函数--不配置PWM",
    "// PWM模式构函数，小提琴舵机 (timer0, 1kHz/10-bit, 跟dog-test一致)": "// PWM模式构函数，小提琴舵机 (timer0, 1kHz/10-bit, 跟dog-test一致)",
    "// PWM模式构函数，自定义定时器和电机timer，如指定": "// PWM模式构函数，自定义定时器和电机timer，如指定",
    "// 舵机 (SG90)": "// 舵机 (SG90)",
    "// 定时器全局只初一次性（50Hz, 14-bit分辨率）": "// 定时器全局只初一次性（50Hz, 14-bit分辨率）",
    "* @brief 设定舵机角度 (0~180度)": "* @brief 设定舵机角度 (0~180度)",
    "* @param angle 目标角度，自动钳位在 [0, 180]": "* @param angle 目标角度，自动钳位在 [0, 180]",
    "将角度转为50Hz PWM占空比 (409~2048对应 0~180度)": "将角度转为50Hz PWM占空比 (409~2048对应 0~180度)",
    "* @brief 平滑扫描：从起始角度到目标角度，在指定时间内完成": "* @brief 平滑扫描：从起始角度到目标角度，在指定时间内完成",
    "* @param from 起始角度": "* @param from 起始角度",
    "* @param to 目标角度": "* @param to 目标角度",
    "* @param duration_ms 完成时（毫秒），每20ms一步": "* @param duration_ms 完成时（毫秒），每20ms一步",
    "// 小鸟跳跃 + 门铃水车驱动 (DRV8833 电机驱动 IN1/IN2)": "// 小鸟跳跃 + 门铃水车驱动 (DRV8833 电机驱动 IN1/IN2)",
    "// config.h 定义: MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP": "// config.h 定义: MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP",
    "// 水车正转: water_bird_->SetSpeed(WATER_WHEEL_SPEED) — IN1导通": "// 水车正转: water_bird_->SetSpeed(WATER_WHEEL_SPEED) — IN1导通",
    "// 水车反转: water_bird_->SetSpeed(-100) — IN2导通": "// 水车反转: water_bird_->SetSpeed(-100) — IN2导通",
    "// MP3播放器 (后台解码与异步播放)": "// MP3播放器 (后台解码与异步播放)",
    "// Mp3Player - 音乐解码播放（异步后台栈）": "// Mp3Player - 音乐解码播放（异步后台栈）",
    "支持: DecodeSingleFile(离线MP3) / PlayUrl(HTTP流MP3) / PlayPcm(原始PCM) / PlayOpus(OGG/Opus)": "支持: DecodeSingleFile(离线MP3) / PlayUrl(HTTP流MP3) / PlayPcm(原始PCM) / PlayOpus(OGG/Opus)",
    "Ducking: AI说话时自动把音乐从100%降至20%，说完了渐恢复至100%": "Ducking: AI说话时自动把音乐从100%降至20%，说完了渐恢复至100%",
    "声音叠加: LoadDogBark加载PCM狗叫至叠加层，可在解码循环中调用": "声音叠加: LoadDogBark加载PCM狗叫至叠加层，可在解码循环中调用",
    "* @brief 析构函数，停止并等待线程退出（5秒超期，超时则强制终止）": "* @brief 析构函数，停止并等待线程退出（5秒超期，超时则强制终止）",
    "// 退去超期5秒则强制退，不recv直接关socket": "// 退去超期5秒则强制退，不recv直接关socket",
    "// 功能: Ducking降音 与 狗叫声叠加（背景音频混音）": "// 功能: Ducking降音 与 狗叫声叠加（背景音频混音）",
    "// 功能: Ducking降音 与 狗叫声叠加（背景音频混音）": "// 功能: Ducking降音 与 狗叫声叠加（背景音频混音）",
    "* @brief 加载狗叫PCM数据至叠加缓冲区": "* @brief 加载狗叫PCM数据至叠加缓冲区",
    "在DecodeSingleFile/PlayUrl的解码循环中检查bark_active_": "在DecodeSingleFile/PlayUrl的解码循环中检查bark_active_",
    "将狗叫PCM叠加到输出缓冲上，实现狗叫和音乐同时播放": "将狗叫PCM叠加到输出缓冲上，实现狗叫和音乐同时播放",
    "* @brief 解码并播放单个离线MP3文件": "* @brief 解码并播放单个离线MP3文件",
    "* @param index MP3文件编号 (对应 0001~0012.mp3、0013为琳达、0015为琳达、0016为园子)": "* @param index MP3文件编号 (对应 0001~0012.mp3、0013为琳达、0015为琳达、0016为园子)",
    "* @return 0=正常停止, 1=持续播放": "* @return 0=正常停止, 1=持续播放",
    "- 跳过ID3v2标签，Minimp3解码，OutputRawPcm输出": "- 跳过ID3v2标签，Minimp3解码，OutputRawPcm输出",
    "- 循环检查bark_active_自动叠加狗叫声": "- 循环检查bark_active_自动叠加狗叫声",
    "- Ducking：AI说话时降至20%音量": "- Ducking：AI说话时降至20%音量",
    "// 从 assets 读取 MP3 文件数据": "// 从 assets 读取 MP3 文件数据",
    "// 跳过 ID3v2 标签：MP3 文件以 \"ID3\" 开头时，前10字节后有一个 sync-safe int 是标签长度）：": "// 跳过 ID3v2 标签：MP3 文件以 \"ID3\" 开头时，前10字节后有一个 sync-safe int 是标签长度：",
    "// 关闭解码器": "// 关闭解码器",
    "// 解码信息": "// 解码信息",
    "// 解码循环": "// 解码循环",
    "// 准备输入帧": "// 准备输入帧",
    "// 准备输出帧": "// 准备输出帧",
    "// 解码": "// 解码",
    "// 首次获取解码信息 — esp_mp3_dec_decode 会通过 dec_info 填出": "// 首次获取解码信息 — esp_mp3_dec_decode 会通过 dec_info 填出",
    "// 有解码出的 PCM 数据，推给音频通道": "// 有解码出的 PCM 数据，推给音频通道",
    "// 降音模式：智能音量管理": "// 降音模式：智能音量管理",
    "// 开始降音": "// 开始降音",
    "// 400ms降至20%": "// 400ms降至20%",
    "// AI在降音期间停止了 — 恢复": "// AI在降音期间停止了 — 恢复",
    "// 400ms恢复到100%": "// 400ms恢复到100%",
    "// AI在恢复期间又开始了说话 — 重新降音": "// AI在恢复期间又开始了说话 — 重新降音",
    "// 在输出缓冲中叠加狗叫数据（做增益处理等）": "// 在输出缓冲中叠加狗叫数据（做增益处理等）",
    "// 等待播放完成，OutputRawPcm 内部阻塞": "// 等待播放完成，OutputRawPcm 内部阻塞",
    "// 移动解码指针": "// 移动解码指针",
    "// 继续拉取并解码下一帧": "// 继续拉取并解码下一帧",
    "// 关闭解码器": "// 关闭解码器",
    "// 恢复小概率的 Opus 解码器错误（如果解码器没容错则恢复之）": "// 恢复小概率的 Opus 解码器错误（如果解码器没容错则恢复之）",
    "//   3. 解析 JSON 响应 (提取 url 字段并重定向)": "//   3. 解析 JSON 响应 (提取 url 字段并重定向)",
    "// 后台流播放实现": "// 后台流播放实现",
    "* @brief 通过HTTP流下载MP3并播放（后台异步任务）": "* @brief 通过HTTP流下载MP3并播放（后台异步任务）",
    "- 流下载：最多4MB缓冲，严格MPEG帧头校验，智能重连 + 重采样转24000Hz": "- 流下载：最多4MB缓冲，严格MPEG帧头校验，智能重连 + 重采样转24000Hz",
    "- Ducking：AI说话时自动降至20%": "- Ducking：AI说话时自动降至20%",
    "- 格式混淆：消费一半缓冲后即循环播放，直到被中断或被打断": "- 格式混淆：消费一半缓冲后即循环播放，直到被中断或被打断",
    "// 严格 MPEG 帧头校验——不只看 FF Ex，还校验 bitrate/samplerate/layer": "// 严格 MPEG 帧头校验——不只看 FF Ex，还校验 bitrate/samplerate/layer",
    "// HTTP 客户端连接": "// HTTP 客户端连接",
    "// 获取长度": "// 获取长度",
    "// 循环下载：如果Content-Length已知，缓冲满即停；否则（分块传输等）边拉边消费": "// 循环下载：如果Content-Length已知，缓冲满即停；否则（分块传输等）边拉边消费",
    "// PSRAM分配失败——降到256KB极限": "// PSRAM分配失败——降到256KB极限",
    "// 重采样缓冲区（用于非16k源 + 立体声转单声道）": "// 重采样缓冲区（用于非16k源 + 立体声转单声道）",
    "// 最坏帧size = 1152, 重采样最大因子 = 24000/8000=3.0, 最大样本数 = 3456": "// 最坏帧size = 1152, 重采样最大因子 = 24000/8000=3.0, 最大样本数 = 3456",
    "// 取4096保证安全余量并方便合并操作": "// 取4096保证安全余量并方便合并操作",
    "// 第一次连接 + 预缓冲：跳过ID3标签 + 向后搜索采样率": "// 第一次连接 + 预缓冲：跳过ID3标签 + 向后搜索采样率",
    "// ---- 跳过 ID3v2 标签 ----": "// ---- 跳过 ID3v2 标签 ----",
    "// ---- 从原始数据扫描 MPEG 帧头提取采样率 ----": "// ---- 从原始数据扫描 MPEG 帧头提取采样率 ----",
    "// ---- 解码循环：消费缓冲并播放 ----": "// ---- 解码循环：消费缓冲并播放 ----",
    "// 解码边消费边播放 + 重采样到24000Hz": "// 解码边消费边播放 + 重采样到24000Hz",
    "// OutputRawPcm内部重采样到24000Hz，如果不重采样会导致音调错误；": "// OutputRawPcm内部重采样到24000Hz，如果不重采样会导致音调错误；",
    "// 第2步：线性插值重采样到kOutRate": "// 第2步：线性插值重采样到kOutRate",
    "// 自适应响应 Ducking 降音": "// 自适应响应 Ducking 降音",
    "// 自适应响应 Ducking 降音": "// 自适应响应 Ducking 降音",
    "// 用解码器返回的 frame_size 而非 raw.consumed 确定帧边界更精确": "// 用解码器返回的 frame_size 而非 raw.consumed 确定帧边界更精确",
    "// 当帧起始位置未发现帧头时，向后搜下一个 MPEG 同步帧并丢弃乱序数据": "// 当帧起始位置未发现帧头时，向后搜下一个 MPEG 同步帧并丢弃乱序数据",
    "// 读取失败或未读完——重新切片从下一个 MPEG 帧头开始": "// 读取失败或未读完——重新切片从下一个 MPEG 帧头开始",
    "// ----- 把缓冲区末尾未消费字节，拼接到下一轮开头 -----": "// ----- 把缓冲区末尾未消费字节，拼接到下一轮开头 -----",
    "// ----- 下载下一批 -----": "// ----- 下载下一批 -----",
    "// carry=0且无帧头则扫描元数据/非帧数据直到下一个严格帧头": "// carry=0且无帧头则扫描元数据/非帧数据直到下一个严格帧头",
    "// PlayPcm: HTTP下载原始PCM — PushRawPcmToPlayback（旧式播放管线，无帧边界问题）": "// PlayPcm: HTTP下载原始PCM — PushRawPcmToPlayback（旧式播放管线，无帧边界问题）",
    "* @brief 通过HTTP下载原始s16le PCM并送到播放管线": "* @brief 通过HTTP下载原始s16le PCM并送到播放管线",
    "* @param url PCM音频URL": "* @param url PCM音频URL",
    "* @return 0=播放成功": "* @return 0=播放成功",
    "- 分块下载：4KB块PushRawPcmToPlayback送播管线": "- 分块下载：4KB块PushRawPcmToPlayback送播管线",
    "- Ducking：AI说话时自动降至20%": "- Ducking：AI说话时自动降至20%",
    "// 分块下载PCM并送播管线": "// 分块下载PCM并送播管线",
    "// 自适应响应 Ducking 降音": "// 自适应响应 Ducking 降音",
    "// 每批量约4个音频帧后交出CPU，防止WiFi拥挤": "// 每批量约4个音频帧后交出CPU，防止WiFi拥挤",
    "* @brief 通过原始BSD Socket下载OGG/Opus音频并送到音频管线": "* @brief 通过原始BSD Socket下载OGG/Opus音频并送到音频管线",
    "* @param url Opus音频URL": "* @param url Opus音频URL",
    "* @return 0=播放成功": "* @return 0=播放成功",
    "- 使用BSD socket直接规避lwip esp_http_client": "- 使用BSD socket直接规避lwip esp_http_client",
    "- Opus路径：OGG解封装 → PushPacketToDecodeQueue（复用Opus解码管线）": "- Opus路径：OGG解封装 → PushPacketToDecodeQueue（复用Opus解码管线）",
    "- PCM路径（legacy）：PushBackgroundAudio（高级背景音频 ring buffer）": "- PCM路径（legacy）：PushBackgroundAudio（高级背景音频 ring buffer）",
    "- Ducking：AI说话时暂停下载数据，等说完了继续下载，说完了恢复": "- Ducking：AI说话时暂停下载数据，等说完了继续下载，说完了恢复",
    "- 自动音量降至65%（减轻AEC回采干扰）": "- 自动音量降至65%（减轻AEC回采干扰）",
    "- 网络不通时优雅降级为串口模式": "- 网络不通时优雅降级为串口模式",
    
    # ---- P2 specific ----
    "// 优先使用背景音频层，有Ducking支持AI混音": "// 优先使用背景音频层，有Ducking支持AI混音",
    "// 构造URL": "// 构造URL",
    "// 原始BSD socket": "// 原始BSD socket",
    "// 先尝试固定点十位IPv4 IP，再尝试DNS解析": "// 先尝试固定点十位IPv4 IP，再尝试DNS解析",
    "// 非阻塞connect+5秒超期（SO_SNDTIMEO在lwip上不一定生效）": "// 非阻塞connect+5秒超期（SO_SNDTIMEO在lwip上不一定生效）",
    "// 设置超期防止goto serial_fallback阻塞启动": "// 设置超期防止goto serial_fallback阻塞启动",
    "// 串口回退路径（网络不通时goto到这里，sock已关闭）": "// 串口回退路径（网络不通时goto到这里，sock已关闭）",
    "// 检查停止标志（由PlayOpus中的Stop()调用），先fread循环停止": "// 检查停止标志（由PlayOpus中的Stop()调用），先fread循环停止",
    "// 从UART0 RX读取Opus数据，初始延迟用串口转换时间差读取": "// 从UART0 RX读取Opus数据，初始延迟用串口转换时间差读取",
    "// 关闭AFE电源+关闭麦克风": "// 关闭AFE电源+关闭麦克风",
    "// 500ms 双次检测，防止串口乱序事件停止": "// 500ms 双次检测，防止串口乱序事件停止",
    "// 发送HTTP GET请求": "// 发送HTTP GET请求",
    "// 读取HTTP响应头": "// 读取HTTP响应头",
    "// 对AI和Opus解码都用同一个播放管线共享": "// 对AI和Opus解码都用同一个播放管线共享",
    "// 在线音乐时音量降至65%，减轻AEC回采干扰": "// 在线音乐时音量降至65%，减轻AEC回采干扰",
    "// Ducking: AI说话时暂停下载数据并推给音频管线": "// Ducking: AI说话时暂停下载数据并推给音频管线",
    "// 检测speaking状态时ResetDecoder会影响解码器": "// 检测speaking状态时ResetDecoder会影响解码器",
    "// 暂停下载数据会使音乐前段出现一小段（5-15ms的AI回复中间有音乐跳音）": "// 暂停下载数据会使音乐前段出现一小段（5-15ms的AI回复中间有音乐跳音）",
    "// 转一次3-5秒的缓冲给5-15ms的AI回复中有音乐跳音": "// 转一次3-5秒的缓冲给5-15ms的AI回复中有音乐跳音",
    "// 恢复下载": "// 恢复下载",
    "// 暂停下载以空出带宽": "// 暂停下载以空出带宽",
    "// 原始s16le PCM: 每2节=1样本": "// 原始s16le PCM: 每2节=1样本",
    "// 原来 while 等待排空会导致 MusicDanceTick 继续跑 ~5 秒（79K 样本 ÷ 16kHz）": "// 原来 while 等待排空会导致 MusicDanceTick 继续跑 ~5 秒（79K 样本 ÷ 16kHz）",
    "// 设备启动时，唤醒词检测必须确保麦克风在未通话时为低功耗状态": "// 设备启动时，唤醒词检测必须确保麦克风在未通话时为低功耗状态",
    "// 长时间背景音乐后，音频播放通道可能停止": "// 长时间背景音乐后，音频播放通道可能停止",
    "// 用esp_codec_dev_read重开通道（重设数据格式）确保正常输出": "// 用esp_codec_dev_read重开通道（重设数据格式）确保正常输出",
    "// 唤醒词检测声底有\"滴滴声\"或者无声问题": "// 唤醒词检测声底有\"滴滴声\"或者无声问题",
    "* @brief 播放闹铃ringtone": "* @brief 播放闹铃ringtone",
    "* @param volume 音量系数 0.0~1.0（开头渐变15%，终值100%）": "* @param volume 音量系数 0.0~1.0（开头渐变15%，终值100%）",
    "交替A5(880Hz)/C#6(1100Hz)双频PCM合成，每响2秒": "交替A5(880Hz)/C#6(1100Hz)双频PCM合成，每响2秒",
    "// 循环生成约100ms播片段，每帧之间检查stop_requested_": "// 循环生成约100ms播片段，每帧之间检查stop_requested_",
    "* @brief 后台播放线程循环": "* @brief 后台播放线程循环",
    "在FreeRTOS总线轮询 pending_track_ / pending_bell_hour_，": "在FreeRTOS总线轮询 pending_track_ / pending_bell_hour_，",
    "播放时暂停AI响应，播对应音频，再恢复AI通话": "播放时暂停AI响应，播对应音频，再恢复AI通话",
    "// 后台播放循环": "// 后台播放循环",
    "// 复位": "// 复位",
    "// 重试": "// 重试",
    "// 停止当前播放": "// 停止当前播放",
    "// 清理 stop_requested_ 并设置目标": "// 清理 stop_requested_ 并设置目标",
    "// 全局音频静音": "// 全局音频静音",
    "// 不要在清置is_playing_=false — 让PlayOpusTask自己清理": "// 不要在清置is_playing_=false — 让PlayOpusTask自己清理",
    "// 反而PlayOpus()中用超可靠等线程退出": "// 反而PlayOpus()中用超可靠等线程退出",
    "// 先音频静音，再超管WaitForPlaybackQueueEmpty()，再放音": "// 先音频静音，再超管WaitForPlaybackQueueEmpty()，再放音",
    "// 停止当前播放": "// 停止当前播放",
    "// 设置播放重复次数": "// 设置播放重复次数",
    "// 全局音频静音": "// 全局音频静音",
    "// 解码失败：MP3最坏情况PCM长度约2.75秒，4秒超时安全": "// 解码失败：MP3最坏情况PCM长度约2.75秒，4秒超时安全",
    "// 打开解码器": "// 打开解码器",
    "// 跳过 ID3v2 标签": "// 跳过 ID3v2 标签",
    "// 逐帧解码+扫描": "// 逐帧解码+扫描",
    "// 关闭解码器": "// 关闭解码器",
    "// LDR 光敏传感器 (ADC oneshot模式)": "// LDR 光敏传感器 (ADC oneshot模式)",
    "* @brief 判断当前是否为黑暗（低于阈值则返回true）": "* @brief 判断当前是否为黑暗（低于阈值则返回true）",
    "* @return true=黑暗, false=光亮": "* @return true=黑暗, false=光亮",
    "// BellSoundPlayer - 布谷鸟/门铃声播放器（通过AI音频系统混音）": "// BellSoundPlayer - 布谷鸟/门铃声播放器（通过AI音频系统混音）",
    "// 不需要 SetOutputMuted，也不需要 vTaskDelay": "// 不需要 SetOutputMuted，也不需要 vTaskDelay",
    "// 直接写 I2S 输出缓冲区，使用 data_if_mutex_ 与 AudioOutputTask 互斥": "// 直接写 I2S 输出缓冲区，使用 data_if_mutex_ 与 AudioOutputTask 互斥",
    "// CuckooStateMachine - 布谷鸟钟状态机": "// CuckooStateMachine - 布谷鸟钟状态机",
    "// 电机总电源 P-MOSFET 控制 (GPIO LOW=ON, HIGH=OFF)": "// 电机总电源 P-MOSFET 控制 (GPIO LOW=ON, HIGH=OFF)",
}

# I'll continue in the next batch. This is a lot of work.
# For now, let me apply what I have and see how many remain.

def fix_file(path, part_name):
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    fixed = 0
    new_lines = []
    for line in lines:
        stripped = line.strip()
        
        # Only process comment lines
        is_comment = stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*')
        if not is_comment:
            new_lines.append(line)
            continue
        
        # Check if in mapping
        if stripped in GARB_MAP:
            repl = GARB_MAP[stripped]
            if repl != stripped:
                prefix = line[:len(line)-len(line.lstrip())]
                new_lines.append(prefix + repl + '\n')
                fixed += 1
                continue
        
        new_lines.append(line)
    
    if fixed > 0:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)
    
    return fixed

total = 0
for p in PARTS:
    path = os.path.join(D, p)
    n = fix_file(path, p)
    total += n
    print(f'{p}: {n} fixed')

print(f'\nTotal: {total} lines fixed')
print('Re-scanning...')
os.system(f'python {os.path.join(BASE, "_scan_comments.py")}')
