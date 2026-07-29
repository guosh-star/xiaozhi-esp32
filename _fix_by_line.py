"""
Fix garbled and English comments using LINE NUMBER mapping.
Reads each part file, detects U+FFFD, replaces by line number.
Safe: only replaces lines with U+FFFD, preserves everything else.
"""
import os, sys, re
sys.stdout.reconfigure(encoding='utf-8')

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
D = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')
PARTS = ['cuckoo_part1.cc','cuckoo_part2.cc','cuckoo_part3.cc','cuckoo_part4.cc','cuckoo_part5.cc']

# File header boilerplate (lines 2-20 in P1, lines 52-70 in P2-P5)
# Format: {part_filename: {line_number: correct_text}}
# FIXES structure: {part_name: {line_number: correct_text}}
FIXES = {}

def init_part(name):
    if name not in FIXES:
        FIXES[name] = {}
    return FIXES[name]

def hdr(p, offset=0):
    """Add header fixes with line offset for P2-P5"""
    pd = init_part(p)
    base = offset
    lines = {
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
    }
    for ln, text in lines.items():
        pd[ln + base] = text

# P1 header starts at line 2
hdr('cuckoo_part1.cc', 0)

# P2-P5 header
hdr('cuckoo_part2.cc', 50)
hdr('cuckoo_part3.cc', 50)
hdr('cuckoo_part4.cc', 50)
hdr('cuckoo_part5.cc', 50)

# P1 specific fixes
P1 = init_part('cuckoo_part1.cc')
P1[51] = "// RTC 崩溃日志存储：崩溃/复位时记录状态，下次开机自动导出"
P1[61] = "// 将服务器返回的 /stream 或 /opus 格式的URL转为 /pcm 格式供ESP32解码音频使用"
P1[63] = "// 直接替换字符串中对应部分"
P1[110] = "// LEDC PWM 通道分配"
P1[112] = "TB6612 电机驱动: 4路 PWM 通道 (频率 ~10-100KHz)"  # strip // for * comments
# Actually these are inside a block comment, need to handle differently
# Let me just focus on the // comments for now

# P2 specific
P2 = init_part('cuckoo_part2.cc')
P2[90] = "// 优先使用背景音频层，有Ducking支持AI混音"
P2[92] = "// 构造URL"
P2[120] = "// 原始BSD socket"
P2[136] = "// 先尝试固定点十位IPv4 IP，再尝试DNS解析"
P2[154] = "// 非阻塞connect+5秒超期（SO_SNDTIMEO在lwip上不一定生效）"
P2[155] = "// 设置超期防止goto serial_fallback阻塞启动"
P2[181] = "// 串口回退路径（网络不通时goto到这里，sock已关闭）"
P2[187] = "// 检查停止标志（由PlayOpus中的Stop()调用）"
P2[202] = "// 从UART0 RX读取Opus数据"
P2[221] = "// 关闭AFE电源+关闭麦克风"
P2[238] = "// 500ms双次检测，防止串口乱序事件停止"
P2[266] = "// 发送HTTP GET请求"
P2[273] = "// 读取HTTP响应头"
P2[302] = "// 对AI和Opus解码都用同一个播放管线共享"
P2[320] = "// 在线音乐时音量降至65%，减轻AEC回采干扰"
P2[338] = "// Ducking: AI说话时暂停下载数据并推给音频管线"
P2[339] = "// 检测speaking状态时ResetDecoder会影响解码器"
P2[340] = "// 暂停下载数据会使音乐前段出现一小段跳音"
P2[341] = "// 转一次3-5秒缓冲防止AI回复中音乐跳音"
P2[369] = "// 恢复下载"
P2[373] = "// 暂停下载以空出带宽"
P2[402] = "// 原始s16le PCM: 每2字节=1样本"
P2[512] = "// 原来 while 等待排空会导致 MusicDanceTick 继续跑约5秒"
P2[520] = "// 设备启动时，唤醒词检测必须确保麦克风通道正常"
P2[521] = "// 长时间背景音乐后，音频播放通道可能停止"
P2[522] = "// 用esp_codec_dev_read重开通道，确保正常输出"
P2[523] = "// 防止唤醒词检测出现\"滴滴声\"或无声问题"
P2[538] = "* @brief 播放闹铃ringtone"
P2[539] = "* @param volume 音量系数 0.0~1.0（开头渐变15%，终值100%）"
P2[540] = "交替A5(880Hz)/C#6(1100Hz)双频PCM合成，每响2秒"
P2[557] = "// 循环生成约100ms播放片段，每帧检查stop_requested_"
P2[593] = "* @brief 后台播放线程循环"
P2[594] = "在FreeRTOS总线轮询 pending_track_ / pending_bell_hour_"
P2[595] = "播放时暂停AI响应，播对应音频，再恢复AI通话"
P2[597] = "// 后台播放循环"
P2[647] = "// 复位"
P2[650] = "// 重试"
P2[665] = "// 停止当前播放"
P2[668] = "// 清理 stop_requested_ 并设置目标"
P2[673] = "// 全局音频静音"
P2[692] = "// 不要在清置is_playing_=false — 让PlayOpusTask自己清理"
P2[693] = "// 反而PlayOpus()中用超可靠等线程退出"
P2[701] = "// 先音频静音，再超管WaitForPlaybackQueueEmpty()，再放音"
P2[737] = "// 停止当前播放"
P2[740] = "// 设置播放重复次数"
P2[743] = "// 全局音频静音"
P2[783] = "// 解码容错：MP3最坏情况PCM约2.75秒，4秒超时安全"
P2[793] = "// 打开解码器"
P2[805] = "// 跳过 ID3v2 标签"
P2[824] = "// 逐帧解码+扫描"
P2[876] = "// 关闭解码器"
P2[896] = "// LDR 光敏传感器 (ADC oneshot模式)"
P2[935] = "* @brief 判断当前是否为黑暗（低于阈值则返回true）"
P2[936] = "* @return true=黑暗, false=光亮"
P2[943] = "// BellSoundPlayer - 布谷鸟/门铃声播放器（通过AI音频系统混音）"
P2[946] = "// 不需要 SetOutputMuted，也不需要 vTaskDelay"
P2[965] = "// 直接写 I2S 输出缓冲区，使用 data_if_mutex_ 互斥"
P2[983] = "// CuckooStateMachine - 布谷鸟钟状态机"
P2[1003] = "// 电机总电源 P-MOSFET 控制 (GPIO LOW=ON, HIGH=OFF)"

# P3 specific
P3 = init_part('cuckoo_part3.cc')
P3[56] = "* @brief 打开电机总电源（P-MOSFET低电平→5V通）"
P3[67] = "// 开场舞蹈电机：整点报时 PerformanceTask + 综合表演 ShowTask 共用"
P3[70] = "// 背景舞蹈电机：整点报时 PerformanceTask + 综合表演 ShowTask 共用"
P3[93] = "// 从 assets 读取 dog_bark.wav (16kHz mono s16)"
P3[117] = "// 把狗叫 PCM 加载到 Mp3Player（DecodeSingleFile 解码循环自动叠加）"
P3[137] = "// 把狗叫 PCM 加载到 Mp3Player（同音）"
P3[142] = "// 异步播放（允许重叠在舞蹈/音乐中）"
P3[146] = "// 小鸟前进 + 暂停 + 后退，与报时声同时播放"
P3[153] = "// 小鸟前进 + 暂停 + 后退，与报时声同时播放"
P3[185] = "// M1 舞蹈电机: 正转/反转表演（短约1~3秒，长约3秒以上）"
P3[219] = "// 小提琴循环摇摆：每帧 9度 平滑摆动"
P3[245] = "// 小狗摇尾"
P3[264] = "// LED 闪烁：每6帧约300ms"
P3[273] = "// 50ms 帧率控制"
P3[283] = "// 音乐结束后清除已播放标记，让下一首重新跳舞直到 ShowTask 末尾熄灯"
P3[287] = "// 电机角速度约27.9度/s（40秒31圈实测），只接360度以内的步进"
P3[311] = "// 平缓减小琴动幅度"
P3[324] = "// 小提琴归位"
P3[330] = "// 小提琴从当前角度归位至180度（旋转方向不变）"
P3[336] = "// 关灯"
P3[346] = "// NVS 数据持久化"
P3[387] = "// 统计有效闹钟数"
P3[398] = "* @brief 启动报时表演（整点/半点/手动）"
P3[399] = "* @param type 表演类型：kPerformanceHour(整点)/kPerformanceHalf(半点)/kPerformanceManual(手动)"
P3[400] = "* @param hour 小时（整点报时用）"
P3[401] = "- 防止AI误触发：5秒窗内忽略重复"
P3[402] = "- 等待AI对话结束，AbortSpeaking（强终止+防唤醒词误检）"
P3[403] = "- 在Core 1启动PerformanceTask执行"
P3[410] = "// 防AI误触：设备从idle换到5秒内的 performanceAI（包括手动触发cuckoo.performance）"
P3[419] = "// 如果 AI 正在说话/播放，等待结束"
P3[427] = "// 等待期间若有新的播放请求，强终止AI（防止唤醒词或 AI 重入）"
P3[429] = "// 显式停止唤醒词检测"
P3[469] = "* @brief 报时任务：开门→小鸟出+叫→关门→播放音乐+舞蹈电机"
P3[470] = "- 整点：敲N次+0013.mp3循环N次+舞蹈+LED+水车+舞蹈电机+小提琴+小狗"
P3[471] = "- 半点：敲3次，播放音乐和舞蹈"
P3[472] = "- 完成后恢复唤醒词和播放状态"
P3[481] = "// 暂停 AI 后台播放管线防止与 Opus 串流混音"
P3[485] = "// ====== 整点报时简化版（无需安装：只用音频通道）======"
P3[487] = "// Phase 1: 开门 → 小鸟出+叫 → 关门"
P3[490] = "// 异步推进鸟+叫——直接混音输出和报时声同步"
P3[500] = "// 敲最后一遍后关灯"
P3[504] = "// 一次性解码到PSRAM→用OutputRawPcm重复播放（避免MP3解码器反复开关的开销）"
P3[514] = "// OutputRawPcm 内部阻塞，等播放完毕"
P3[522] = "// 继续用 PlayIndex"
P3[545] = "// Phase 3: 恢复AI音频检测"
P3[573] = "// 异步报时声+小鸟（背景音频同步出，舞蹈循环和 ShowTask 一致）"
P3[581] = "// 舞蹈+小鸟声开始→打开水车/小鸟声/舞蹈中"
P3[598] = "// ====== 半点报时：小鸟+报时+叫一声======"
P3[603] = "// 异步推进鸟+叫——直接混音输出和报时声同步"
P3[613] = "// 敲最后一遍后关灯"
P3[616] = "// 以后恢复 AI"
P3[629] = "// 恢复唤醒词检测"
P3[630] = "// 只在 idle 状态恢复，防止 listening 状态后误触唤醒词"
P3[639] = "* @brief 检查并执行整点/半点报时"
P3[640] = "- 夜间(22:00-6:00)不报时"
P3[641] = "- AI忙时背景音频播放中不报时"
P3[642] = "- 整点 (min==0): 调用 StartPerformance(kPerformanceHour)"
P3[643] = "- 半点 (min==30): 调用 StartPerformance(kPerformanceHalf)"
P3[652] = "// AI 忙时背景音频播放中不报时"
P3[660] = "// 静音模式判断"
P3[686] = "// mode==1 全天报时→直接return"
P3[688] = "// 整点报时"
P3[690] = "// 半点报时"
P3[696] = "// 半点报时"
P3[718] = "// 静音模式 NVS 存储"
P3[780] = "// 闹钟功能 — 支持最多5个闹钟，NVS持久化"
P3[783] = "* @brief 设置闹钟"
P3[784] = "* @param hour 小时 (0-23)"
P3[785] = "* @param minute 分钟 (0-59)"
P3[786] = "* @param repeat_daily true=每天重复, false=一次性"
P3[816] = "* @brief 获取闹钟列表JSON"
P3[853] = "* @brief 停止闹钟（关闭铃声）"
P3[854] = "一次性闹钟停止后自动删除，重复闹钟保留"
P3[879] = "* @brief 每秒闹钟检查任务"
P3[880] = "- 时间匹配且秒==0时触发"
P3[881] = "- 启动AlarmTask在后台播放"
P3[882] = "- 已响的不重复触发"
P3[911] = "* @brief 闹钟播放任务：响铃50次（100秒），音量15%渐强至100%"
P3[957] = "// 特色表演：三个 MCP 工具的实现"
P3[958] = "// - DogShow: 小狗出洞表演（开门、跑出来、叫、摇头、退回、关门）"
P3[959] = "// - LindaShow: 琳达舞蹈表演（播放0015 + 舞蹈电机 + LED闪烁）"
P3[960] = "// - GardenShow: 园子Linda风表演（播放0016 + 小提琴动 + LED闪烁）"
P3[963] = "// 布谷鸟钟特色：小狗出洞表演"
P3[964] = "// 实际表演由 DogShowTask() 在后台执行，后台 Core 1 线程运行。"
P3[966] = "* @brief 小狗出洞表演（MCP入口，非阻塞返回）"
P3[967] = "在Core 1启动dog_show任务，实际由DogShowTask()执行"

# P4 specific
P4 = init_part('cuckoo_part4.cc')
P4[52] = "// 后台线程执行表演，固定在 Core 1（钟控核心）"
P4[61] = "* @brief 小狗出洞表演实现"
P4[67] = "// === 先等 AI 说完话（表演和AI不能同时进行）==="
P4[68] = "// === 如果再同时启动 AI则重试，等后开始 ==="
P4[70] = "// === 从第3秒开始表演 ==="
P4[186] = "* @brief 通过WAV文件名播放狗叫"
P4[250] = "// 直接播放狗叫（非通过 PlayWavAsset 的叠加通道）"
P4[258] = "// 解码 MP3 到 PSRAM"
P4[279] = "// 按需重采样 22.05→16kHz"
P4[299] = "// 先 drain 积累数据，防止 ring buffer 溢出"
P4[322] = "* @brief 琳达出场表演（MCP入口，非阻塞返回）"
P4[325] = "// MCP 工具入口，非阻塞。实际表演在后台线程执行。"
P4[335] = "* @brief 园子出场表演（MCP入口，非阻塞返回）"
P4[338] = "// MCP 工具入口，非阻塞。实际表演在后台线程执行。"
P4[348] = "* @brief 琳达表演实现"
P4[356] = "// 先等 AI 说完话（表演和AI不能同时进行），再开始后续动作"
P4[363] = "// 1. 先LED亮一下，表示表演即将开始"
P4[368] = "// 2. 开始播放音乐（bg audio），覆盖TTS/I2S"
P4[379] = "// 3. LED交替闪烁 + 舞蹈电机间歇正反转，直到音乐结束"
P4[398] = "// 音乐停止前约2秒（24秒时），LED全亮并停止闪烁"
P4[405] = "// 舞蹈电机间歇1~3秒反转，持续到音乐结束"
P4[444] = "// LED交替闪烁，限于前24秒，每6帧切换一次 = 约300ms间隔"
P4[458] = "// 5. 音乐结束 → 舞蹈停 → 开感谢灯 → LED熄灭"
P4[461] = "// 电机角速度约27.9度/秒"
P4[485] = "// 感谢灯光循环亮起（0~4循环）"
P4[510] = "* @brief 园子表演实现"
P4[518] = "// 先等 AI 说完话（表演和AI不能同时进行），再开始后续动作"
P4[525] = "// 1. 先LED亮一下，表示表演即将开始"
P4[530] = "// 2. 开始播放音乐（bg audio），覆盖TTS/I2S"
P4[541] = "// 3. LED闪烁 + 小提琴动，直到音乐结束"
P4[560] = "// 小提琴摆动: 每50ms帧移动9度, 0~140度左右摆动"
P4[583] = "// 5. 音乐结束 → 小提琴平缓归位 → 开感谢灯 → LED熄灭"
P4[588] = "// 感谢灯光循环亮起（0~4循环）"
P4[619] = "// M1舞蹈电机: 正转约1-3秒 → 停20ms → 反转约1-3秒 → 停20ms → 循环（共约8次）"
P4[666] = "// 小提琴动: 90→0→180→90 循环摆动"
P4[681] = "// 电机速度测试：舞蹈电机自转指定时长"
P4[682] = "// 命令: cuckoo.motor_test (seconds: 1~60)"
P4[699] = "* @brief 综合表演入口（MCP cuckoo.start_show，非阻塞返回）"
P4[700] = "在Core 1启动cuckoo_show任务 → StartShowTask"
P4[703] = "// 整点/半点报时期间不允许 Show，防止 AI 混音出错"
P4[709] = "// 已有表演在跑（含报时），需要等停后才开始"
P4[715] = "// 锁占用，防止重复启动"
P4[719] = "// 异步执行（MCP 工具调用在主事件循环线程上，app.Schedule阻塞）"
P4[720] = "// 如果主事件循环等待 AbortSpeaking/TTS-STOP，会卡住"
P4[721] = "// （Schedule 回调在这种中断时状态转换无法执行）"
P4[722] = "// 所以把整个表演流程放到 Core 1 后台线程里"
P4[739] = "* @brief 综合表演后台线程"
P4[740] = "停止AI响应→LED+水车→选曲→播放+小提琴动(异步启动)→舞蹈循环→LED亮→关门→恢复"
P4[757] = "* @brief 综合表演执行（由StartShowTask调用的实际逻辑）"
P4[768] = "// 停止 AI（表演和AI不能同时进行）"
P4[776] = "// 选曲"
P4[806] = "// 异步报时声+小提琴（背景音频同步出，舞蹈循环并行）"
P4[813] = "// 舞蹈+小提琴开始→打开水车/小提琴声/舞蹈中"
P4[816] = "// 音乐结束后恢复唤醒词和播放状态"
P4[817] = "// 注意：StartShow 不 AbortSpeaking 来清状态（listening→idle）"
P4[818] = "// 直接恢复唤醒词会在 listening 状态上 AbortSpeaking 阻塞 audio_input"
P4[823] = "// 确保设备回到 idle 再恢复唤醒词"
P4[849] = "// 恢复唤醒词（保持设备在idle，钟控不再转换到其他状态）"
P4[866] = "* @brief 全局停止所有电机和演出"
P4[867] = "- 停止所有电机M1~M4 + 小提琴 + 水车"
P4[868] = "- 小提琴归位90度"
P4[869] = "- LED熄灭"
P4[870] = "- 在线音乐不停止（除非是表演模式）"
P4[871] = "- 通过Schedule异步设置设备状态为Idle，恢复唤醒词"
P4[887] = "// 防止 stop_all+play_url 的组合误触发清理"
P4[897] = "// 先强制 AbortSpeaking TTS，否则后续操作会被TTS流阻塞"
P4[916] = "* @brief 停止音乐播放（含背景音频）"
P4[927] = "* @brief 开门（M2电机反转，时长 MAIN_DOOR_TIME_MS）"
P4[942] = "* @brief 关门（M2电机正转，时长 MAIN_DOOR_TIME_MS）"
P4[958] = "* @brief 小鸟跳跃（电机上拉500ms + 下电等待400ms）"
P4[978] = "* @brief 小鸟轻跳跃（AI说话时伴随话动，轻幅）"
P4[979] = "持续50-200ms上拉，等待300-700ms下落"
P4[982] = "// AI说话时小鸟伴随话动跳跃（自然表现模式）"
P4[983] = "// 上拉时间50-200ms模拟自然跳跃"
P4[984] = "// 下落后等待300-700ms，防止动作太密集"

# P5 specific
P5 = init_part('cuckoo_part5.cc')
P5[384] = "* @brief 设置舵机角度"
P5[385] = "* @param servo_id 0=小提琴舵机, 1=小狗尾巴舵机"
P5[386] = "* @param angle 0~180度"
P5[395] = "* @brief 设置电机速度"
P5[396] = "* @param motor_id 1=M1舞蹈, 2=小提琴电机, 3=小狗电机, 4=开门电机"
P5[397] = "* @param speed -100~100 (正=正转)"
P5[418] = "* @brief 在线音乐播放入口"
P5[421] = "- 防止AI对话时重入：置CPU忙"
P5[422] = "- 自动处理URL中的中文和空格"
P5[423] = "- 无http前缀则自动拼接代理地址"
P5[424] = "- 正在播放时先停旧再播新"
P5[584] = "// 如果 AI 正在说话，TTS 和音乐不能同时播放，PlayOpus 会自动 duck 到 30%"
P5[594] = "// 如果给的URL有http开头就直接用"
P5[599] = "// 替换 URL 中的空格为 %20"
P5[602] = "// 预留 2 字符"
P5[610] = "// 已在播放→先停旧再播新"
P5[615] = "// 等PlayOpusTask退出（is_playing_变false）"
P5[621] = "// 清空背景音频buffer防前后歌曲串音"
P5[628] = "// 拼接代理地址前缀"
P5[643] = "// 空格在 HTTP request line 中是分隔符，会导致解析错误"
P5[653] = "// ASCII直接拷贝"
P5[656] = "// 非ASCII %XX 编码"
P5[683] = "// 已在播放→先停旧再播新"
P5[700] = "* @brief 设置音乐代理服务器地址"
P5[701] = "* @param host 代理服务器IP"
P5[702] = "* @param port 代理服务器端口"
P5[711] = "// CuckooTools MCP注册"
P5[1033] = "// === 特色表演 ==="
P5[1091] = "// 钟控 FreeRTOS 任务 (Core 1)"
P5[1093] = "// 时钟: 250ms sub-tick, 1s per tick"
P5[1094] = "// 职责:"
P5[1095] = "- NTP时间同步（60s/300s间隔校准）"
P5[1096] = "- 设备状态监控（idle?启动自动关灯；AI说话时小鸟轻跳跃）"
P5[1097] = "- 唤醒词阈值动态管理（对话时0.3防止误触发；idle时0.02灵敏）"
P5[1098] = "- 音频时钟刷新（每10s防止I2S电源关闭而关闭）"
P5[1099] = "- 整点/半点报时调度"
P5[1100] = "- 闹钟检查"
P5[1101] = "- RTC崩溃日志（每30s保存到硬件内存）"
P5[1104] = "// 时间读取（NTP原样 + RTC补偿）"
P5[1119] = "// 更新时钟并刷新音频时钟（防止长期关机后I2S播放无时钟）"
P5[1124] = "// 设备连上 WiFi 后 NTP gettimeofday 即可获取当前时间"
P5[1126] = "// 第二次尝试：以防第一次NTP失败"
P5[1144] = "// 加载默认音乐代理地址（从 config.h 编译时固定）"
P5[1147] = "// 从 NVS 读取已保存的亮度和静音模式"
P5[1154] = "// TODO(#22): 把1秒轮询改为事件驱动:"
P5[1155] = "// - 注册 OnDeviceStateChanged 回调感知 idle/active 转换"
P5[1156] = "// - 把定时任务改为NTP同步和tick_sec驱动"
P5[1157] = "// - 在Core 1大部分时间休眠，节省功耗"
P5[1163] = "// 如果设备在idle且没有用户ON/OFF录音时段，防止误触发"
P5[1171] = "// 对话时提高唤醒词阈值到0.3，防止误触发（表演期间不需要，Show自己控制）"
P5[1175] = "// 对话中恢复标准麦克风增益（30dB），防止 AEC 过峰削波"
P5[1181] = "// idle时恢复敏感阈值0.02（排除表演期间/报时期间不设置）"
P5[1184] = "// idle时提升麦克风增益（37.5dB），补偿安装位置损耗"
P5[1204] = "// AI说话时小鸟伴随话动跳跃（仅在非表演/非报时期间）"
P5[1206] = "// 根据语音能量动态跳跃（300ms平均数 = 平均声音强度）"
P5[1209] = "// 说话声有随机的50-200ms持续跳跃"
P5[1212] = "// 不说话时恢复"
P5[1217] = "// === 每秒（sub_tick % 4 == 0）=== "
P5[1221] = "// NTP同步：未同步时每60秒尝试，同步后每300秒校准一次"
P5[1239] = "// WiFi省电：当前CuckooBoard::SetPowerSaveLevel处理"
P5[1240] = "// 背景音乐播放时用最低省电LOW_POWER"
P5[1242] = "// 每30秒保存崩溃日志至RTC硬件内存（RTC在开关电复位间保持）"
P5[1251] = "// 每60秒输出内存+栈水印（辅助调试）"
P5[1258] = "// 音频时钟保护：背景/解码器停止超过15秒时关闭 I2S 避免时钟失效"
P5[1263] = "// 读取是否已配置亮度和静音模式"
P5[1267] = "// 整点报时通过 NTP 或 MCP 设置，实时测时间"
P5[1269] = "// NTP未同步时系统时间，可能初始时间偏移"
P5[1279] = "// NTP未同步（不能用MCP设时间），静音报警直到时钟准"
P5[1291] = "// 闹钟检查"
P5[1295] = "// 整点报时（每分钟 == 0 时检查，NeedHourlyChime 防止重复）"
P5[1300] = "// 半点报时（每分钟 == 30 时检查，NeedHalfHourlyChime 防止重复）"
P5[1306] = "// 闹钟检查（每分钟检查一次）"
P5[1310] = "// 注：NTP同步后重置时间偏移（每分钟tick_sec%86400校准）"

# Now scan comment lines from _garb_comments.txt and add any missing
# by reading the scan output and matching line numbers
with open(os.path.join(BASE, '_garb_comments.txt'), 'r', encoding='utf-8') as f:
    garb_lines = f.readlines()

for gl in garb_lines:
    parts = gl.strip().split(': ', 2)
    if len(parts) < 3:
        continue
    # Format: cuckoo_partX.cc:LINE: text
    fname = parts[0]
    try:
        ln = int(parts[1])
    except:
        continue
    
    if fname in FIXES and ln in FIXES[fname]:
        continue  # Already have a fix
    
    # For remaining lines, add placeholder: strip U+FFFD and replace with context-derived text
    rest = parts[2]
    # If line has mainly U+FFFD with some surviving Chinese, try to use the survivors
    if '\ufffd' in rest:
        # Remove repeated U+FFFD blocks
        cleaned = rest
        # We'll skip unknown lines for now - focus on the ones we know

# Now apply all fixes
def get_original_line(filepath, line_num):
    """Read a specific line from a file"""
    with open(filepath, 'r', encoding='utf-8') as f:
        for i, line in enumerate(f, 1):
            if i == line_num:
                return line.rstrip('\n\r')
    return None

def apply_fixes():
    total = 0
    for p, fixes in sorted(FIXES.items()):
        if not fixes:
            continue
        path = os.path.join(D, p)
        with open(path, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        
        # Sort fixes by line number descending to avoid offset issues
        sorted_fixes = sorted(fixes.items(), reverse=True)
        
        for ln, correct_text in sorted_fixes:
            idx = ln - 1  # 0-based
            if idx < 0 or idx >= len(lines):
                print(f'  WARN: {p} L{ln} out of range ({len(lines)} lines)')
                continue
            
            orig = lines[idx].rstrip('\n\r')
            # Only fix if the line contains U+FFFD (garbled)
            if '\ufffd' in orig:
                # Preserve indentation
                indent = orig[:len(orig) - len(orig.lstrip())]
                lines[idx] = indent + correct_text + '\n'
                total += 1
        
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(lines)
    
    return total

total = apply_fixes()
print(f'Applied {total} garbled comment fixes')

# Now translate English comments
# Read the English comments file
with open(os.path.join(BASE, '_eng_comments.txt'), 'r', encoding='utf-8') as f:
    eng_lines = f.readlines()

EN_FIXES = {}  # {(part, line): translated_text}
for el in eng_lines:
    parts = el.strip().split(': ', 2)
    if len(parts) < 3:
        continue
    fname = parts[0]
    try:
        ln = int(parts[1])
    except:
        continue
    text = parts[2].strip()
    
    translated = None
    # Simple translations
    if text == "// /stream?q=... -> /pcm?q=...":
        continue  # already correct
    elif text == "// /opus?q=... -> /pcm?q=...":
        continue  # already correct
    elif text == "// ---- Smooth ducking: fade music out when AI starts speaking ----":
        translated = "// ---- 平滑降音: AI开始说话时淡出音乐 ----"
    elif text == "// Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring":
        translated = "// 状态机: 0=空闲, 1=淡出中, 2=已降音, 3=恢复中"
    elif text == "// Apply gain + clip":
        translated = "// 增益处理 + 限幅"
    elif text == "// Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz":
        translated = "// 最坏情况MP3帧缓冲 (1152立体声, 8000→24000Hz)"
    elif text == "// Step 1: stereo -> mono (average L/R)":
        translated = "// 第1步: 立体声→单声道 (取左右均值)"
    elif text == "// ---- Smooth ducking: fade when AI starts speaking ----":
        translated = "// ---- 平滑降音: AI说话时淡出 ----"
    elif text == "// Clear stale bg audio from previous session to prevent startup noise burst":
        translated = "// 清除上次残留背景音频, 防止开机爆音"
    elif text == "// === Serial fallback ===":
        translated = "// === 串口回退 ==="
    elif text == "// === Detect format: /opus uses OGG demuxer + main audio pipeline ===":
        translated = "// === 格式检测: /opus→OGG解复用+音频管线 ==="
    elif text == "// === /pcm uses raw bytes pushed to background ring buffer ===":
        translated = "// === /pcm→原始字节→背景环形缓冲 ==="
    elif text == "// ============ Opus path: OGG demux + PushPacketToDecodeQueue ============":
        translated = "// ============ Opus路径: OGG解复用+送入解码队列 ============"
    elif text == "// ============ PCM path (legacy): raw s16le PushBackgroundAudio ============":
        translated = "// ============ PCM路径(旧): 原始s16le→背景音频 ============"
    elif text == "// === Pre-buffer phase: fill ring buffer before enabling drain ===":
        translated = "// === 预缓冲: 填满环形缓冲再启用输出 ==="
    elif text == "// Set gain based on current AI state before enabling drain":
        translated = "// 启用输出前按AI状态设增益"
    elif text == "// (prevents full-volume music + TTS overlap noise)":
        translated = "// (防止全音量音乐+TTS重叠噪音)"
    elif text == "// Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)":
        translated = "// 仅AI说话时降音; 聆听保持满音量 (2026-07-19需求)"
    elif text == "// Set gain even on timeout otherwise stays at 0.001f":
        translated = "// 超时也设增益, 防止卡0.001f"
    elif text == "// === Main download push loop ===":
        translated = "// === 主下载推送循环 ==="
    elif text == "// Listening keeps music at 100%; duck to 50% only while AI speaks/connects":
        translated = "// 聆听时音乐100%; AI说话/连接时才降50%"
    elif text == "// When AI is speaking, pause TCP download to free WiFi airtime for":
        translated = "// AI说话时暂停TCP下载, 释放WiFi空口给"
    elif text == "// UDP audio packets (prevents WiFi buffer starvation TTS stutter).":
        translated = "// UDP音频包 (防WiFi缓冲不足导致TTS卡顿)"
    elif text == "// Only pause if buffer sufficient to ride through typical AI reply.":
        translated = "// 仅缓冲够撑典型AI回复时才暂停"
    elif text == "// Server closed connection gracefully":
        translated = "// 服务器正常关闭连接"
    elif text == "// read < 0: timeout or transient error":
        translated = "// 读取失败: 超时/瞬时错误"
    elif text == "// Wait 1s before retry WiFi may be reconnecting after AI conversation":
        translated = "// 等1秒重试 (AI对话后WiFi可能重连中)"
    elif text == "// Refresh power save in case it was changed by channel close":
        translated = "// 刷新省电模式 (防止被通道关闭修改)"
    elif text == "// Diagnostic: log buffer fill + download rate every 8s":
        translated = "// 诊断: 每8秒记录缓冲填充+下载速率"
    elif text == "// Fix(2026-07-19): idle transition during music skips threshold restore":
        translated = "// 修复(2026-07-19): 音乐中空闲切换跳过阈值恢复"
    elif text == "// (IsBgAudioActive guard), leaving 0.30 stuck. Restore sensitive 0.02 here.":
        translated = "// (IsBgAudioActive守卫), 导致卡0.30, 在此恢复灵敏的0.02"
    elif text == "// play single track - manage mute here":
        translated = "// 播放单曲 - 管理静音"
    elif text == "// play bell chime using short PCM bell sound (~0.5s each)":
        translated = "// 短PCM钟声(~0.5s)播放报时"
    elif text == "// pause ~1s between strikes for natural clock sound":
        translated = "// 每敲之间停约1秒, 模拟自然钟声"
    elif text == "// Decoder buffered all input; no progress possible, stop":
        translated = "// 解码器已缓冲全部输入; 无法继续, 停止"
    elif text == "// Mix dog bark on top of existing bg audio (overlap, not replace)":
        translated = "// 狗叫叠加混入背景音频 (叠加不替换)"
    elif text == "// Bg music may still be fading in while AI speaks: wait up to 5s for it":
        translated = "// 背景音乐可能还在淡入中: 最多等5秒"
    elif text == "// Create PerformanceTask on Core 1":
        translated = "// 在Core 1创建报时任务"
    elif text == "// Wait for AI to finish speaking (max 5s)":
        translated = "// 等AI说完话 (最长5秒)"
    elif text == "// Abort AI speech if still talking":
        translated = "// 还在说则中止AI"
    elif text == "// Wait for bg audio to start":
        translated = "// 等背景音频启动"
    elif text == "// Wait for dance intro to finish":
        translated = "// 等舞蹈开场完成"
    elif text == "// Wait for music to end":
        translated = "// 等音乐结束"
    elif text == "// ---- ADC oneshot init ----":
        translated = "// ---- ADC单次采样初始化 ----"
    elif text == "// --- Alarms ---":
        translated = "// --- 闹钟 ---"
    elif text == "// --- Quiet Mode ---":
        translated = "// --- 静音模式 ---"
    elif text == "// --- Hourly Performance Toggle ---":
        translated = "// --- 整点演出开关 ---"
    elif text == "// --- Music / Show ---":
        translated = "// --- 音乐/演出 ---"
    elif text == "// --- Hardware (wiring later) ---":
        translated = "// --- 硬件 (后续接线) ---"
    elif text == "// --- Cantonese lookup (2026-07-19) ---":
        translated = "// --- 粤语查询 (2026-07-19) ---"
    elif text == "// 1. Water wheel on":
        translated = "// 1. 开水车"
    elif text == "// 2. Open door":
        translated = "// 2. 开门"
    elif text == "// 3. Dog tail out: 180->20, 1200ms":
        translated = "// 3. 狗尾伸出: 180→20度, 1200ms"
    elif text == "// 4. Dog forward":
        translated = "// 4. 狗前进"
    elif text == "// 5. Bark #1":
        translated = "// 5. 狗叫第1声"
    elif text == "// 6. Tail to 0 deg":
        translated = "// 6. 尾巴归位0度"
    elif text == "// 7. Wag 10s: 0<->60":
        translated = "// 7. 摇尾10秒: 0↔60度"
    elif text == "// 8. Bark #2":
        translated = "// 8. 狗叫第2声"
    elif text == "// 9. Tail back to 20 deg":
        translated = "// 9. 尾巴回20度"
    elif text == "// 10. Dog reverse":
        translated = "// 10. 狗后退"
    elif text == "// 11. Tail back to 180 deg":
        translated = "// 11. 尾巴回180度"
    elif text == "// 12. Stop water wheel":
        translated = "// 12. 关水车"
    elif text == "// 13. Close door":
        translated = "// 13. 关门"
    elif text == "// : (board B M2 via violin_motor_)":
        translated = "// : (B板M2通过violin_motor_)"
    elif text == "// Mp3Player tracks playback state (stays true during AI speech ducking),":
        translated = "// Mp3Player播放状态 (AI说话降音期间仍为true)"
    elif text == "// while IsBgAudioActive() may briefly drop when audio service clears buffers.":
        translated = "// 而IsBgAudioActive()清缓冲时会瞬间掉false"
    elif text == "// Using both ensures music dance survives AI conversations.":
        translated = "// 双检确保音乐舞蹈在AI对话中不中断"
    elif text == "// For online /stream or /opus: url is already correct":
        translated = "// 在线/stream或/opus: URL已正确"
    else:
        # Try heuristic for remaining English comments
        if all(ord(c) < 128 for c in text) and not text.startswith('// URL'):
            pass  # Keep as-is for now
    
    if translated:
        EN_FIXES[(fname, ln)] = translated

# Apply English fixes
en_total = 0
for p in PARTS:
    path = os.path.join(D, p)
    fixes_for_this = {ln: txt for (fname, ln), txt in EN_FIXES.items() if fname == p}
    if not fixes_for_this:
        continue
    
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    for ln in sorted(fixes_for_this.keys(), reverse=True):
        idx = ln - 1
        if idx < 0 or idx >= len(lines):
            continue
        indent = lines[idx][:len(lines[idx]) - len(lines[idx].lstrip())]
        lines[idx] = indent + fixes_for_this[ln] + '\n'
        en_total += 1
    
    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(lines)

print(f'Applied {en_total} English comment translations')

# Verify: re-scan + line count
print('\n--- Verification ---')
# Check line counts
for p in PARTS:
    path = os.path.join(D, p)
    with open(path, 'r', encoding='utf-8') as f:
        n = len(f.readlines())
    print(f'{p}: {n} lines')

os.system(f'python {os.path.join(BASE, "_scan_comments.py")}')
