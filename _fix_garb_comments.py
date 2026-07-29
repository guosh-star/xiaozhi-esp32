"""
Fix all garbled Chinese comments in all 5 part files.
Uses a comprehensive replacement table mapped from garbled UTF-8 to correct Chinese.
"""
import os, re

PARTS_DIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
PARTS = ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']

# Comprehensive garbled -> correct mapping table
# Sorted by length (longest first to avoid partial matches)
REPLACEMENTS = [
    # === P1 header comments (L2-20) ===
    ("// ӿ (Cuckoo Controller)", "// 布谷鸟钟控制 (Cuckoo Controller)"),
    ("// ܸ", "// 功能概述"),
    ("// - 4·ֱ + 1·񶯵 + ˮ", "// - 4路直流电机 + 1路步进电机 + 水车"),
    ("// - ƣСٶСβͶ", "// - 4个舵机(鸟门/小狗/小提琴/狗尾巴) + 2个普通舵机"),
    ("// - LED ˸· LED /", "// - LED 灯2路 LED_A / LED_B"),
    ("// - Ƶţ MP3/Opus/PCM 룬HTTP أDucking ܻ", "// - 音频播放: MP3/Opus/PCM解码 + HTTP流下载 + AI Ducking 混音控制"),
    ("// - /㱨ʱſ + СԾ + Ƚ + ֣", "// - 布谷鸟/整点报时: 开门 + 小鸟跳跃 + 鸟叫 + 转圈"),
    ("// - ۺϱݣ赸 + С + С + ˮ +  + LED", "// - 综合表演: 舞蹈 + 小提琴 + 水车 + 小狗 + LED"),
    ("// - ɫ MCP ߣDogShowСLindaShowմGardenShow԰ӣ", "// - 角色 MCP 工具: DogShow(小狗), LindaShow(琳达), GardenShow(花园)"),
    ("// - 幦ܣNVS ־û5֧ÿظ", "// - 用户功能: NVS 日志/用户偏好5项/每日重复"),
    ("// - ҹģʽ22:00-6:00", "// - 夜间模式: 22:00-6:00 禁止报时"),
    ("// - ֲţQQִOpus/PCM ʽأ", "// - 在线音乐: QQ音乐代理/Opus/PCM 格式解码推送"),
    ("// - MCP עᣨ21ߣͨ MCP Э鹩 AI ģ͵ã", "// - MCP 注册(21个工具), 通过 MCP 协议供 AI 大模型调用"),
    ("// мܹ", "// 线程架构"),
    ("// - Core 0: ѴʡTTSOpus룩", "// - Core 0: 语音(唤醒词/TTS/Opus解码)"),
    ("// - Core 1: ӿcuckoo_clock_task250ms tickʱͱʱ", "// - Core 1: 钟控(cuckoo_clock_task/250ms tick/定时和报时)"),
    ("// - /ʱͨ xTaskCreatePinnedToCore  Core 1 ִ", "// - 音乐/报时通过 xTaskCreatePinnedToCore 到 Core 1 执行"),

    # === RTC crash log (L51-57) ===
    ("// RTC ڴ棺/λ״̬´οԶʱ", "// RTC 内存: 记录崩溃位置/状态, 下次重启可读取"),
    ("uint32_t magic;        // ħ 0xCAFEBABE ʾЧ", "uint32_t magic;        // 魔数 0xCAFEBABE 表示数据有效"),
    ("uint32_t tick_sec;     // ǰ", "uint32_t tick_sec;     // 崩溃前运行秒数"),
    ("uint8_t  dev_state;    // 豸״̬", "uint8_t  dev_state;    // 设备状态"),
    ("uint8_t  music_active; // Ƿڲ", "uint8_t  music_active;  // 音乐是否在播放"),
    ("uint32_t free_heap;    // ʣڴ", "uint32_t free_heap;    // 剩余堆内存"),

    # === ConvertToPcmUrl (L61-100) ===
    ("//  /stream  /opus ʽURLתΪ /pcm ʽESP32Ƶʹ", "// 将 /stream 或 /opus 格式URL转换为 /pcm 格式供ESP32音频使用"),
    ("// 滻ַ", "// 替换字符串"),
    ('// "/stream" = 7ַ, "/pcm" = 4ַ,  "?..."  pos+7', '// "/stream" = 7字符, "/pcm" = 4字符, 所以 "?..." 从 pos+7 开始'),
    ('// "/opus" = 6ַ, "/pcm" = 4ַ,  "?..."  pos+5', '// "/opus" = 6字符, "/pcm" = 4字符, 所以 "?..." 从 pos+5 开始'),
    ('// ҲǰбܵAIʱ "stream?q= "  "opus?q= "', '// 也可能是没有前面的/的AI请求格式 "stream?q= " 或 "opus?q= "'),
    ('// "stream" = 6ַ, "pcm" = 3ַ,  "?..."  pos+6', '// "stream" = 6字符, "pcm" = 3字符, 所以 "?..." 从 pos+6 开始'),
    ('// "opus" = 4ַ, "pcm" = 3ַ,  "?..."  pos+4', '// "opus" = 4字符, "pcm" = 3字符, 所以 "?..." 从 pos+4 开始'),

    # === LEDC PWM channels (L110-115) ===
    ("// LEDC PWM ͨ", "// LEDC PWM 通道分配"),
    ("// TB6612 : 4· PWM ͨ (Ƶ ~10-100KHz)", "// TB6612 直流电机: 4路 PWM 通道 (频率 ~10-100KHz)"),
    ("// : 2· PWM ͨ (Ƶ 50Hz)", "// 舵机: 2路 PWM 通道 (频率 50Hz)"),
    ("// ŵ L9110S: 1· PWM ͨ (Ƶ ~1-10KHz)", "// 步进电机 L9110S: 1路 PWM 通道 (频率 ~1-10KHz)"),
    ("// 7·ͨ", "// 共7路通道"),

    # === GPIO & Motor constructors (L121-147) ===
    ("// GPIOʼ֧GPIOֱPWM˫ģʽ", "// GPIO模式: 只支持GPIO直接输出(无PWM/双极性模式)"),
    ("// GPIOģʽ캯ֱPWM", "// GPIO模式构造函数(直接GPIO输出无PWM)"),
    ("// PWMģʽ캯С (timer0, 1kHz/10-bit, dog-testһ)", "// PWM模式构造函数(默认 timer0, 1kHz/10-bit, 与dog-test一致)"),
    ("// PWMģʽ캯Զ嶨ʱŵtimerָ", "// PWM模式构造函数(自定义定时器和timer指定)"),

    # === Servo (L189) ===
    ("// ʱȫֻʼһΣ50Hz, 14-bitֱʣ", "// 静态全局只初始化一次, 50Hz, 14-bit精度(ESP-IDF默认)"),

    # === Water wheel & Dog walk (L249-252) ===
    ("// С + ˮ (DRV8833  IN1/IN2)", "// 小狗行走 + 水车 (DRV8833 双路 IN1/IN2)"),
    ("// ˮת: water_bird_->SetSpeed(WATER_WHEEL_SPEED)  IN1", "// 水车正转: water_bird_->SetSpeed(WATER_WHEEL_SPEED) 驱动 IN1"),
    ("// ˮת: water_bird_->SetSpeed(-100)  IN2", "// 水车反转: water_bird_->SetSpeed(-100) 驱动 IN2"),

    # === MP3 Player (L255-270) ===
    ("// MP3 (첽)", "// MP3 播放器(多线程)"),
    ("// Mp3Player - 벥첽棩", "// Mp3Player - 异步播放(多线程/环形缓冲)"),
    ("// ֧: DecodeSingleFile(MP3) / PlayUrl(HTTPMP3) / PlayPcm(ԭʼPCM) / PlayOpus(OGG/Opus)", "// 支持: DecodeSingleFile(MP3) / PlayUrl(HTTP流MP3) / PlayPcm(原始PCM) / PlayOpus(OGG/Opus)"),
    ("// Ducking: AI˵ʱԶ100%20%Ϊ˵𽥻ָ100%", "// Ducking: AI说话时自动从100%降到20%, 结束后渐进恢复100%"),
    ("// л: LoadDogBarkPCMصڽѭе", "// 狗叫: LoadDogBarkPCM加载到叠加混音缓冲区"),
    ("// 5˳recvرsocket", "// 最多5次重试/超时后关闭socket"),
    ("vTaskDelay(pdMS_TO_TICKS(50));  // ܹ5", "vTaskDelay(pdMS_TO_TICKS(50));  // 总共5次重试"),

    # === Ducking (L288-290) ===
    ("// ܣDucking  Ƶ·", "// 混音相关: Ducking 状态和音频路径"),

    # === DecodeSingleFile (L357-906) ===
    ("//  assets ȡ MP3 ļ", "// 从 assets 分区读取 MP3 文件"),
    ("//  ID3v2 ǩMP3 ļ \"ID3\" ͷʱǰ10ֽں sync-safe int Ǳǩȣ", "// 跳过 ID3v2 标签(MP3 文件 \"ID3\" 开头时跳过10字节头+sync-safe int 编码的标签长度)"),
    ("if (skip < mp3_size - 1024) {  // ȷ㹻", "if (skip < mp3_size - 1024) {  // 确保还有足够数据"),
    ("// رս", "// 解码结束"),
    ("// Ϣ", "// 错误信息"),
    ("// ѭ", "// 重试循环"),
    ("// ׼֡", "// 准备帧"),
    ("// ״λȡϢ  esp_mp3_dec_decode ͨ dec_info", "// 错误状态位可从 esp_mp3_dec_decode 返回的 dec_info 获取"),
    ("// н PCM ݣƸͨ", "// 拥有 PCM 数据, 通过混音通道输出"),
    ("// ģʽܣȫ", "// Ducking模式混音: 降低音量"),
    ("// ʼ", "// 淡出开始"),
    ("// AIڽڼֹͣ  ָ", "// AI在说话期间停止后立即恢复"),
    ("// 400msָ100%", "// 400ms内恢复100%"),
    ("// AIڻָڼֿʼ˵  ½", "// AI在恢复期间又开始说话, 重新降低"),
    ("// йݴȥ", "// 所有数据已处理完"),
    ("// ȴɣOutputRawPcm", "// 等待完成, 调用 OutputRawPcm"),
    ("// ƶָ", "// 动态恢复"),
    ("consecutive_errors = 0;  // ɹô", "consecutive_errors = 0;  // 成功, 清零错误计数"),
    ("// ǰ֡", "// 跳过当前帧"),
    ("// ָСǵ Opus Ǳ϶ָ", "// 动态恢复与之前的 Opus 逻辑一致(淡入恢复)"),
    ("//   3.  JSON Ӧ (ȡ url ֶκ)", "// 步骤 3.  解析 JSON 响应 (提取 url 字段后拼接)"),
    ("// ̨", "// 输出到控制台"),
    ("// ϸ MPEG ֡ͷУ鲻 FF Ex У bitrate/samplerate/layer", "// 解析 MPEG 帧头: 校验同步字 FF Ex, 提取 bitrate/samplerate/layer"),
    ("// HTTP ͻ", "// HTTP 客户端"),
    ("// 壺Content-Length֪Σ߽֡λᣩ", "// 批量模式: Content-Length已知时(整体帧边界更可靠)"),
    ("batch_size = content_length; // ȫһ", "batch_size = content_length; // 全部一次下载"),
    ("// PSRAMʧܡ˵256KB", "// PSRAM失败回退, 限制256KB"),
    ("// ز + תã", "// 采样率转换: 重采样 + 转换"),
    ("// =1152, ز=24000/8000=3.0, =3456", "// 每帧采样数=1152, 重采样率=24000/8000=3.0, 输出=3456"),
    ("// 4096ȫϲ", "// 4096字节对齐(完全合并)"),
    ("// һ + ԤID3ǩ +", "// 第一块 + 预留ID3标签 + 预下载"),
    ("// ----  ID3v2 ǩ ----", "// ---- 跳过 ID3v2 标签 ----"),
    ("if (mp3_start > batch_len - 1024) mp3_start = 0; // ǩ̫/ݲ㣬ͷʼ", "if (mp3_start > batch_len - 1024) mp3_start = 0; // 标签太大/数据不足, 从头开始"),
    ("// ---- ԭʼɨ MPEG ֡ͷȡ ----", "// ---- 原始扫描 MPEG 帧头提取 ----"),
    ("if (lay != 1) continue; // ֻҪ Layer 3", "if (lay != 1) continue; // 只要 Layer 3"),
    ("// ---- ѭʽ----", "// ---- 循环模式 ----"),
    ("//  + ز24000Hz", "// 采样率转换: 输入 + 重采样24000Hz"),
    ("// OutputRawPcmڲزᵼң", "// OutputRawPcm内部重置会导致丢失同步"),
    ("// 2ԲֵزkOutRate", "// 2倍线性插值重采样到kOutRate"),
    ("// ӦDucking", "// 应用Ducking增益"),
    ("// ԻӦDucking", "// 动态应用Ducking"),
    ("// ýص frame_size  raw.consumed ȷ߽֡羫ȷ", "// 使用已解码的 frame_size 替换 raw.consumed 来确保帧边界更精确"),
    ("if (c == 0) c = raw.consumed;  // ˻", "if (c == 0) c = raw.consumed;  // 首次回退"),
    ("// ʼ֡ͷһ MPEG ֹͬ֡ѭ", "// 从起始帧头扫描下一个 MPEG 同步帧(禁止越界)"),
    ("else { rem = 0; break; }  // δҵͬ֡ͷcarryʣ", "else { rem = 0; break; }  // 未找到同步帧头, 携带剩余"),
    ("// ʧδѡϸһ MPEG ֡ͷ", "// 失败(未找到下一个合格 MPEG 帧头)"),
    ("// ----- ĩβδֽڣƴӵͷ -----", "// ----- 块末尾剩余字节, 拼接到开头 -----"),
    ("// ----- һ -----", "// ----- 下一个块 -----"),
    ("// carry=0ҷ֡ͷɨmetadata/֡ݵһϸ֡ͷ", "// carry=0且找不到帧头: 扫描metadata/帧数据直到下一个合格帧头"),
    ("rem = 0; // Ч֡ͷ", "rem = 0; // 无效帧头, 跳过"),

    # === PlayPcm (L929-1011) ===
    ("// PlayPcm: HTTPԭʼPCM  PushRawPcmToPlaybackԭŹߣ֡⣩", "// PlayPcm: HTTP流原始PCM -> PushRawPcmToPlayback(原始播放通道, 非MP3解帧)"),
    ("// ֿPCM벥Ŷ", "// 高优先级PCM解码播放队列"),
    ("const size_t CHUNK = sizeof(self->output_buf_);  // С", "const size_t CHUNK = sizeof(self->output_buf_);  // 输出缓冲区大小"),
    ("size_t samples = read / 2;  // 16λ", "size_t samples = read / 2;  // 16位采样"),
    ("// ÿԼ4ƵóCPUֹWiFi", "// 每解码约4个音频块释放一次CPU防止WiFi饥饿"),

    # === Part 2 (P2) ===
    ("stop_requested_ = false;  //  Stop() õı־", "stop_requested_ = false;  // 清除 Stop() 调用的标志"),
    ("// ʹñƵ㣬DuckingAI", "// 使用默认音频输出, Ducking跟随AI"),
    ("// ԭʼBSD socket", "// 原始BSD socket"),
    ("// ȳԵʮIPٳDNS", "// 先尝试直接IP再尝试DNS"),
    ("// connect+5볬ʱSO_SNDTIMEOlwipϲЧ", "// connect+5秒超时(SO_SNDTIMEO在lwip上不生效)"),
    ("// ֹgoto serial_fallbackʼ", "// 防止goto serial_fallback未初始化"),
    ("// ڻ·ͨgotosockѹرգ", "// 在线路更换时通过goto跳转, sock已关闭"),
    ("// ֹͣ־PlayOpusеStop()ãfreadѭ", "// 先清除停止标志(PlayOpus中的Stop()调用fread循环)"),
    ("// UART0 RXȡOpusݣʼӳôתȡ", "// UART0 RX接收Opus数据, 开启乒乓缓冲区自动接收"),
    ("// ֹAFEԴر˷", "// 停止AFE音频防止回声干扰"),
    ("if (app.GetDeviceState() == kDeviceStateConnecting) break;  // Ѵʴ stop", "if (app.GetDeviceState() == kDeviceStateConnecting) break;  // 唤醒词触发 stop"),
    ("// 500ms˫ؼ⣬ֹѵ¼ֹͣ", "// 500ms握手去抖动, 防止唤醒事件误停止"),
    ("// ȡHTTPӦͷ", "// 提取HTTP响应头"),
    ("// AIOpusͬһŶй", "// AI的Opus也用同一个播放通道"),
    ("// ֲʱ65%AECزɸ", "// 串口模式65%音量(AEC回声补偿高)"),
    ("// Ducking: AI˵ʱݵƸ", "// Ducking: AI说话时数据音量降低"),
    ("// speaking״̬ʱResetDecoder˽", "// speaking状态变化时ResetDecoder重置解码器"),
    ("// ݻʹǰһСΣ5-15AIظм", "// 不活跃用户时前一小段(5-15帧)AI语音缓冲区"),
    ("// һ3-5ӵĸ5-15AIظм", "// 注: 3-5帧的延迟不影响5-15帧AI语音缓冲区"),
    ("// ָ", "// 恢复标志"),
    ("// ȴſ", "// 等待打开完成"),
    ("// ԭʼs16le PCM: ÿ2ֽ=1", "// 原始s16le PCM: 每2字节=1个"),
    ("// 豸ʱѴʼ⣬ȷ˷ͨ", "// 设备未唤醒时延迟初始化, 确保麦克风通道"),
    ("// ʱ䱳ƵźƵ·ֹͣ", "// 长时间背景音频信号无音频路径导致停止"),
    ("// esp_codec_dev_readع/ݵ", "// esp_codec_dev_read阻塞/数据丢失"),
    ("// Ѵʼ\"\"ʵ", "// 唤醒词初始化\"空\"实现"),
    ("// ɲ100ms鲥ţÿ֮stop_requested_", "// 插入100ms静音帧, 每次循环检查stop_requested_"),
    ("// ֹͣǰ", "// 先停止当前播放"),
    ("//  stop_requested_ Ŀ", "// 设置 stop_requested_ 目标"),
    ("// ȷѴ", "// 确保已唤醒"),
    ("// Ҫڴis_playing_=false  PlayOpusTaskԼ", "// 不需要等待is_playing_=false  PlayOpusTask自行退出"),
    ("// PlayOpus()Կɿصȴ˳", "// PlayOpus()内部可控的等待退出"),
    ("// ƵУþWaitForPlaybackQueueEmpty()ٷ", "// 音频停止时用WaitForPlaybackQueueEmpty()同步"),
    ("// 䣺MP3PCMԼ2.754ǳȫ", "// 缓冲: MP3替换PCM预取(约2.754秒, 足够安全)"),
    ("// 򿪽", "// 解码方向"),
    ("// νɨ", "// 线性扫描"),
    ("// LDR 贫 (ADC oneshotģʽ)", "// LDR 光敏电阻 (ADC oneshot模式)"),
    ("return raw;  // 0-4095 (12-bit), =ֵ, =ֵ", "return raw;  // 0-4095 (12-bit), 亮=高值, 暗=低值"),
    ("// BellSoundPlayer - ͨAIƵϵͳ", "// BellSoundPlayer - 通过AI音频系统播放"),
    ("// Ҫ SetOutputMutedҲҪ vTaskDelay", "// 不需要 SetOutputMuted也不需要 vTaskDelay"),
    ("// ֱд I2S ֱ꣬ʹ data_if_mutex_  AudioOutputTask", "// 直接写入 I2S 缓冲区, 使用 data_if_mutex_ 与 AudioOutputTask 协作"),
    ("// CuckooStateMachine - ״̬", "// CuckooStateMachine - 状态机核心"),
    ("violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),  // Сٵ (B M2, GPIO18/45)", "violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),  // 小提琴电机 (A路 M2, GPIO18/45)"),

    # More replacements from P2 onwards - will continue in next batch
]

# Sort by length descending to avoid partial matches
REPLACEMENTS.sort(key=lambda x: len(x[0]), reverse=True)

total = 0
for part_name in PARTS:
    path = os.path.join(PARTS_DIR, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    for old, new in REPLACEMENTS:
        if old in content:
            content = content.replace(old, new)
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        changes = len([r for r in REPLACEMENTS if r[0] in original])
        print(f'{part_name}: {changes} replacements')
    else:
        print(f'{part_name}: no changes')

print(f'Done. Processed {len(PARTS)} files.')
