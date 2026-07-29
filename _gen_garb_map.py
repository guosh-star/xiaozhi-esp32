#!/usr/bin/env python
"""Generate _garb_map.txt with all garbled -> correct Chinese mappings."""
import os

out = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_map.txt'

# Read all remaining garbled phrases
with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_remaining.txt', 'r', encoding='utf-8') as f:
    phrases = [l.strip() for l in f.readlines()[4:] if l.strip()]

print(f'Building map for {len(phrases)} phrases...')

# Comprehensive mapping
M = {
    # === Clock / Chime ===
    ' - /�㨨ʱ': ' - 整点/半点报时',
    '/�㨨ʱ': '整点/半点报时',
    '�㨨ʱ': '整点报时',
    '�㨨ʱ3��ν': '半点报时3次鸣叫',
    '�㨨ʱÿ == 0 ʱ NeedHourlyChime �ظ�': '整点报时(每 == 0 时, NeedHourlyChime 防重复)',
    '�㨨ʱÿ == 30 ʱ NeedHalfHourlyChime �ظ�': '半点报时(每 == 30 时, NeedHalfHourlyChime 防重复)',
    '�㨨ʱÿһ��': '混合报时(每激活一次)',
    '====== �㨨ʱ+С+һ ======': '====== 整点报时+小狗+小提琴 ======',
    '====== �㨨ʱ򻯰�棨�ް�װֻ��Ƶ�̣======': '====== 整点报时+简单表演(无包装, 只有音频) ======',

    # === Door ===
    '�Ŵ�': '鸟门打开',
    '�Źر�': '鸟门关闭',
    'һ�ٹ�': '最后一声后关门',
    '壨��ʱ���ڲ�': '脉冲跳跃(中平同时内部计数)',
    '壩ͣ�ٿ�ʼ': '清理背景, 等待开始',

    # === AI / Voice ===
    'AI æʱ�򱳾��Ƶ���в�ʱ': 'AI 正忙时背景音频无播放时间',
    'AI � �� TTS ͬʱ�У�PlayOpus �Զ� duck 30%': '等待 AI 说完 TTS 同时(PlayOpus 自动 duck 到 30%)',
    'AI �˵/�ȴ�': '等待 AI 说完/等待完成',
    'AI �˵��ݺ�ͬʱ���У���ֵ��ӿ�': '等待 AI 说完(表演和音乐同时开始, 有打断控制)',
    'AI �� �ղŶ��з�ֹ Opus': '等待 AI 停止(播放队列中停止 Opus)',
    'AI˵ʱС� �滰��Ծ��ģʽ': 'AI说话时小鸟会说话+弹跳模式',
    'AI˵ʱС� �滰��Ծ���ڱޱ�ʱ': 'AI说话时小鸟会说话弹跳(在后台报时期间)',
    'AI��ݺ�ͬʱ': '等待 AI 停止(表演和音乐同时开始)',
    'AI󴥷��豸մ�idle5 performance��AI�ڻ�ʱ��Զ�cuckoo.performance': 'AI未触发/设备处于idle超过5分钟→自动performance(AI在后台时自动cuckoo.performance)',
    '===\x02 AI \u02e8\u03b9\u013b\u0373\x02===': '=== 等待 AI 说完(表演和音乐同时开始) ===',
    '=== ͬʱ AIֵ\u023b\u0385Ȼ ===': '=== 同时 AI 打断控制逻辑 ===',
    '=== 3ʼ ===': '=== 步骤3: 音乐开始 ===',
    '=== ÿ��sub_tick % 4 == 0��===': '=== 每4个sub_tick(sub_tick % 4 == 0) ===',
    '=== ɫ ===': '=== 角色工具 ===',
    'Phase 1: Ŵ  С+  Źر': 'Phase 1: 鸟门打开 → 小鸟+鸟叫 → 鸟门关闭',
    'Phase 3: ָAI\u03b7': 'Phase 3: 恢复AI音频',
    'ָAIƵ': '恢复AI音频',
    'ָ AI': '恢复 AI 语音',
    '�Ѵʼ\u02b8\u02e8 EnableVoiceProcessing  ȳResetDecoder TTS': '恢复唤醒词初始化( EnableVoiceProcessing + ResetDecoder + TTS)',
    'ָ\ue000Ѵʼ\u02b8\u02e8 EnableVoiceProcessing   ResetDecoder  TTS': '恢复唤醒词初始化( EnableVoiceProcessing + ResetDecoder + TTS)',
    'ָ\ue000Ѵ\u037d\x04豸idle\u023b\u0385\u0289ת': '恢复唤醒词等待设备idle后钟控接管',
    '�� idle ״̬Żָ listening ״̬󴥷�Ѵ': '只在 idle 状态才恢复 listening 状态(后触发唤醒词)',

    # === Stops ===
    'stop_all+play_url طŴ': '触发 stop_all+play_url 后重新打开大门',
    'ж\u05af AbortSpeaking TTS Ȼ': '直接 AbortSpeaking 中断 TTS 然后清理',

    # === Motors / Servos ===
    'MotorPowerOff();  // Ĭ϶ϵ磬100K  5V': 'MotorPowerOff();  // 默认断电, 100K 下拉电阻保持 5V 稳定',
    'GPIOֱȫѹ': 'GPIO直接全正压',
    'M1 赸: ת/ת棨1~3룬>3': 'M1 舞蹈电机: 正转/反转随机运行1~3秒, >3秒时换向',
    'M1赸: ת1-3  ͣ20ms  ת1-3  ͣ20ms  ѭ8': 'M1舞蹈: 正转1-3秒 → 停20ms → 反转1-3秒 → 停20ms → 循环8次',
    'С': '小提琴',
    'Сٵ (B M2, GPIO18/45)': '小提琴电机 (A路 M2, GPIO18/45)',
    'Сٶ: 90018090 ѭڶ': '小提琴动作: 90°→0°→180°→90° 循环摆动',
    ': ÿ50ms֡ƶ9, 0~140Ұڶ': '小提琴: 每50ms帧步进9°, 0~140°左右摆动',
    'ӵǰǶƽص90': '从当前角度平顺回到90度',
    'н\u036a²\u013f279/s40s31Ȧʵ⣩ֻ360ڵĲ': '自转角度补偿 279°/s, 40秒约31圈(实际更多), 只补 360° 内的偏移',
    'н\u036a²\u013f279/s': '自转角度补偿 279°/s',
    'תۼʱ': '正转累计时间',
    'ٶȲԣ赸תָ': '角度补偿: 舞蹈旋转归位',
    'ƽСת': '平衡小车旋转',
    'ȷԴȶ': '确保电源稳定',
    'С': '小狗',
    'Сǰ  ͣ  Уͱͬʱ': '小狗前进 1000ms: 停止水车, 小狗和鸟同步',
    'С˻': '小狗后退',
    'СӵǰǶȹλ180㣨䣩': '小狗从当前角度回归180度(复位)',
    'Сѭÿ֡ 9 ƽڶ': '小提琴循环: 每帧步进 9 度平滑摆动',
    'Сҡβ': '小狗摇尾巴',

    # === Water bird ===
    'ˮ IN1=HIGH': '水车 IN1=HIGH',
    'if (water_bird_) water_bird_->Stop();  // ͣˮͷGPIO39ͻ': 'if (water_bird_) water_bird_->Stop();  // 先停水车(拉低GPIO39避免冲突)',
    'gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // ͨ磬С񵯳': 'gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // 反向通电, 小鸟弹出',
    'gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // ϵ磬С': 'gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // 反向断电, 小鸟归位',
    'int cooldown = 300 + (esp_random() % 401);  // 300-700msȴʱ': 'int cooldown = 300 + (esp_random() % 401);  // 随机300-700ms冷却时间',
    '50-200msģȻԾ': '50-200ms脉冲然后弹跳',
    'ȴ300-700ms̫': '等待300-700ms不要太快',

    # === Dance / Performance ===
    '赸+С̿ʼͿ/СУ': '舞蹈+小狗开始/停止音乐控制',
    '赸1~3룬Ȼ': '舞蹈运行1~3秒, 然后反向',
    '赸У �㨨ʱ PerformanceTask + \u06fa\u03f1 ShowTask �ã': '舞蹈/半点和整点报时 PerformanceTask + 综合表演 ShowTask 都在这里',
    '첽+С赸ѭ': '异步音乐+小狗舞蹈循环',
    '첽+С赸ѭ ShowTask һ£': '异步音乐+小狗舞蹈循环(与 ShowTask 一致)',
    '첽ţ赸/ֲУ': '异步绑定: 用于舞蹈/在线音乐中的狗叫',
    '첽ƽ+Ͷͬ': '异步音乐绑定+鸟叫同步',
    '첽ִУMCP ߵѭ\u036b\u03b5\u026aϣ�app.Schedule': '异步执行(MCP 工具的循环逻辑冲突app.Schedule)',
    '��ñ\u036e\x04ֹ\u03b7ظ': '占用标志, 防止重复启动',
    'ʵʱ DogShowTask() ִУ Core 1 ̨\u03a1\u0373': '实际在 DogShowTask() 中执行, Core 1 后台任务。',
    'MCP ڣءʵʱںִ̨С': 'MCP 入口(调度的), 实际在后台任务执行',

    # === Shows ===
    '1. LEDһʾݼʼ': '1. LED一闪表示表演开始',
    '2 Įַ': '替换 2 字符',
    '3. LED˸ + Сٶֱֽ': '3. LED闪烁 + 小提琴动作(直到结束)',
    '3. LED˸ + 赸תֽ': '3. LED闪烁 + 舞蹈电机(旋转+换向)',
    '24LEDȫ赸ֱֽ': '24秒后: LED全亮, 舞蹈直到结束',
    'ǰ24룺LED˸ + 赸': '前24秒: LED闪烁 + 舞蹈',
    '5. ֽ  Сƽ  Ÿл  LEDϨ': '5. 结束阶段: 小提琴平复 → 大门切换 → LED熄灭',
    '5. ֽ  赸ͣ  Ÿл  LEDϨ': '5. 结束阶段: 舞蹈停止 → 大门切换 → LED熄灭',
    'LED ˸ÿ6֡ ~300ms': 'LED 闪烁: 每6帧切换一次 ~300ms',
    'LED˸ǰ24룬ÿ6֡лһ = Լ300ms': 'LED闪烁前24秒, 每6帧切换一次 = 约300ms',
    '50ms ֡ʿ': '50ms 帧速率',
    'ֽһֱŹرպ ShowTask ĩβϨ': '结束阶段一直发光(不闪烁), ShowTask 末尾统一熄灭',
    'лֻţ0~4ѭ': '狗叫切换编号, 0~4循环',

    # === Show roles ===
    'DogShow: С\u02f5\u0169\x01\u026f\u0143\u030b\u03b3С\u03c9\u037e\u038a\u030b\u02b1\u0383\u0255\u030b\u026b\u0372': 'DogShow: 小狗表演(出大门→前进→摇尾→后退→进门)',
    'LindaShow: մ\u0255赸\u0169\x010015 + 赸 + LED˸': 'LindaShow: 琳达舞蹈表演(0015 + 舞蹈 + LED闪烁)',
    'GardenShow: ԰Linda\u0169\x010016 + Сٶ + LED˸': 'GardenShow: 花园(类似Linda)(0016 + 小提琴动作 + LED闪烁)',
    'DogShow()  MCP ڣز': 'DogShow() 即 MCP 入口(不走调度)',
    'ɫ\u0169\x01\u03f1 MCP ߵʵ֣': '角色表演: MCP 工具的底层实现',

    # === Dog / Kids ===
    'Mid-music transition: kids became active \u02e5 bring dog out': 'Mid-music transition: kids became active → bring dog out',
    'Mid-music transition: kids became resting \u02e5 stop & put dog away': 'Mid-music transition: kids became resting → stop & put dog away',

    # === LED ===
    '// === 大门+小狗：只有大门关着才执行 ===': '// === 大门+小狗: 只有大门关着才执行 ===',

    # === Audio ===
    'OutputRawPcm ģŷ': 'OutputRawPcm 默认播放方式',
    'һ�\u036d\x02뵽PSRAMOutputRawPcmظţMP3򿪵Ķ': '最后一声直接进入PSRAM→OutputRawPcm(播放队列, MP3已打开的输出)',
    'drain  \u0169\ue004����ֹ ring buffer \u0372\u016c': '先 drain 旧数据, 防止 ring buffer 残留',
    'ز 2205016000': '重采样 22050→16000',
    'volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% ǿ 100% (ǰ10)': 'volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% 渐强到 100% (前10帧)',
    'ֱӲŹУ PlayWavAsset': '直接播放队列, 使用 PlayWavAsset',
    '�Ñ\u018f\u03eeƴ': '使用字符串拼接',

    # === Restore ===
    'if (mp3_) mp3_->SetDisableDucking(false);  // ָ duck': 'if (mp3_) mp3_->SetDisableDucking(false);  // 恢复 duck',
    'if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // ָ duck': 'if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // 恢复 duck',
    'if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // ��ֲ': 'if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // 音乐不让duck',
    'app.GetAudioService().SetOutputMuted(false);  // ָAIƵ': 'app.GetAudioService().SetOutputMuted(false);  // 恢复AI音频',
    'ȷ豸ص idle ٻָ': '确保设备回到 idle 再恢复',

    # === NVS / Config ===
    'NVS ӳ־û': 'NVS 映射日志/用户偏好',
    'NVS ѱ;ģʽ': '从 NVS 加载保存的静音模式',
    'ģʽ NVS �洢': '静音模式 NVS 存储',
    'ģʽж': '静音模式判断',
    'Ĭִַ config.h ʱ̶': '默认不再执行(仅 config.h 静态配置)',
    '幦  ֧5ӣNVS־û': '用户功能: 最多支持5项, NVS日志/用户偏好',

    # === Quiet mode ===
    'ȫ쾲': '全天静音',
    'ھ': '光控静音',
    'ҹ: 22~6': '夜间: 22~6',
    'mode==1 ȫ챨ʱreturn': 'mode==1 全天报时(不return)',
    'ȡҹģʽ': '读取夜间模式',

    # === Cuckoo clock task ===
    'ӿ FreeRTOS  (Core 1)': '钟控 FreeRTOS 任务 (Core 1)',
    'ְ:': '职责:',
    '- NTPʱͬ60s/300sУ׼': '- NTP时间同步(60s/300s校准)',
    '- 豸״̬أidle?ԶţAI˵ʱСԾ': '- 设备状态监控(idle→自动放音乐, AI说话时小鸟弹跳)',
    '- Ѵֵ̬Ի0.3ֹ󴥷idleʱ0.02': '- 唤醒词阈值动态切换(0.3防止误触发, idle时0.02)',
    '- Ƶʱˢ£ÿ10sֹI2SԴر�': '- 音频定时刷新(每10s防止I2S自动关闭)',
    '- Ӽ': '- 用户标记',
    '- RTC־ÿ30s浽ڴ棩': '- RTC日志(每30s写入内存)',
    '- /�㨨ʱ': '- 整点/半点报时',
    'TODO(#22): 1ѯΪ¼:': 'TODO(#22): 一次轮询改为事件驱动:',
    '- ע OnDeviceStateChanged ص idle/active ת': '- 注册 OnDeviceStateChanged 回调处理 idle/active 切换',
    '- ʱNTPͬtick_sec': '- 定时NTP同步tick_sec',
    '- Core 1󲿷ʱʡ': '- Core 1后台部分可废弃',
    '豸idleûAI¼ʱڷ󴥷': '设备idle(没有AI事件)时降低触发阈值',
    'Իлָ׼˷棨30dB AEC Ŵ': '动态切换恢复标准麦克风增益(30dB AEC 窗口)',
    'Ի߻Ѵֵ0.3ֹ󴥷ڼ䲻裬ShowԼƣ': '动态切换唤醒词阈值(0.3防止误触发, 期间不切换, Show会自己管理)',
    'idleʱ˷棨37.5dBװλ': 'idle时麦克风增益(37.5dB标准设置)',
    'idleʱֵָ0.02ڼ/Ÿڼ䲻': 'idle时恢复阈值0.02(在音乐/报时期间不操作)',

    # === RTC / Time ===
    '���ȡԭ + RTC': '读取原因 + RTC',
    'ʱˢƵʱֹڼI2SŹʱ': '定时刷新音频时钟(防止长时无播放I2S关闭)',
    'WiFi  NTP gettimeofday ɻȡǰʱ': '从 WiFi + NTP gettimeofday 获取当前时间',
    '2001Ժ˵ NTP ͬ': '2001年以后说明 NTP 已同步',
    'NTPδͬMCPʱ䣩˵ʱӵ': 'NTP未同步(用MCP设置的时间), 模拟时钟推进',
    'NTPͬʱϵͳʱ䣬ʱƯ': 'NTP已同步(系统时间, 时钟不漂移)',
    'NTPͬδͬʱÿ60ԣͬÿ300У׼һ': 'NTP同步(未同步时每60秒尝试, 已同步每300秒校准一次)',
    'WiFiʡCuckooBoard::SetPowerSaveLevelش': 'WiFi节能(CuckooBoard::SetPowerSaveLevel不处理)',
    '��ֲʱLOW_POWER': '串口模式保持LOW_POWER',
    'ÿ30뱣־RTCڴ棨RTCڿŹλԱ': '每30秒保存日志到RTC内存(RTC在开关机后可供复位比较)',
    'ÿ60+ջˮλڱ': '每60秒监控栈水位(定时报告)',
    '注NTP\u036c²ʱÿtick_sec%86400У׼': '注意NTP同步时每tick_sec%86400校准',

    # === Audio refresh ===
    'ƵԴ/ 15 ر I2S »ʧЧ': '音频自动恢复: 15秒刷新 I2S "永不关闭"策略',

    # === Idle detection ===
    '5봰': '5秒窗口',
    '״̬ص idle': '状态切换回 idle',
    'ڼֹΪѴʻ AI': '避免中断当前唤醒词或 AI 对话',
    'ʽѴʼ⣨ɰ汾 EnableVoiceProcessing ܲ߼': '延迟唤醒词初始化(新版本 EnableVoiceProcessing 管理逻辑)',

    # === Misc ===
    '¼ܱʱ㣩': '记录总报时次数(统计用)',
    'ý': '让脚步动作完成',
    'Ƚ(~1.76s)': '等叫声结束(~1.76s)',
    'ͳЧʱ': '统一有效时间',
    'ֶ': '小时分字段',
    'Ͱ': '旧模式清理',
    '�ɺɾ': '任务完成后删除自身',
    '���ִ̨б�\u0169\ue000̶ Core 1ӿغģ': '后台执行表演, 固定 Core 1(钟控核心)',
    'бУܾ': '已有表演在运行, 拒绝',
    'бУܾظ': '已有表演在运行, 拒绝重复启动',
    'Schedule صŲ϶ӣTTS-STOP ״̬ת޷ִУ': 'Schedule 回调没有中断机制, TTS-STOP 状态转换被迫执行',
    '԰ŵ Core 1 ̨\u03a1\u0373': '所以放到 Core 1 后台任务。',
    '�\u0385ȡ AbortSpeaking/TTS-STOP': '回调中等待 AbortSpeaking/TTS-STOP',
    'ע⣺StartShow  AbortSpeaking �ϰ�״̬ listening idle': '注意: StartShow 等待 AbortSpeaking 状态从 listening 变 idle',
    '�\u034a\u0262ӻָᵼ»Ѵ listening �ϰ�״̬ AbortSpeaking  audio_input': '直接恢复会导致唤醒词 listening 状态 AbortSpeaking 被 audio_input 覆盖',

    # === Music/Play ===
    'CuckooTools MCPע': 'CuckooTools MCP注册',
    'URL еĿոΪ %20': '将 URL 中的空格替换为 %20',
    'URLhttpͷֱʹ': 'URL可能包含http头, 直接使用',
    'ASCIIֱӸ': 'ASCII直接复制',
    '�ո HTTP request line \u0374Ƿָ': '构建 HTTP request line + 必要头部',
    'ձƵbufferֹǰ׻': '清空音频buffer防止上一首残留',
    'ڲţͣɸٷ¸': '若在播放, 停止后再发下一首',
    'ȾPlayOpusTask˳is_playing_false': '等待PlayOpusTask退出(is_playing_变false)',
    '先停止当前播放Լ2루24ʱLEDȫֹͣ˸': '先停止当前(约2秒, 24秒时LED全亮停止闪烁)',

    # === Schedules ===
    'ʱͨ NTP  MCP ãڲʱ': '报时(优先 NTP 而非 MCP 设置, 用内部时钟)',

    # === Compiler notes ===
    '// 第0步：统一路径格式，把 /opus 或 /stream 都转成 /pcm': '// 第0步: 统一路径格式, 把 /opus 或 /stream 都转成 /pcm',
    '// 第2步：构造完整URL，直接用原始路径不转换': '// 第2步: 构造完整URL, 直接用原始路径不转换',
    '// /pcm 本身就能返回JSON或PCM，无需改路径': '// /pcm 本身就能返回JSON或PCM, 无需改路径',
    '// 第3步：没有 Content-Length = 流式传输 = 音频（不是JSON），跳过': '// 第3步: 没有 Content-Length = 流式传输 = 音频(不是JSON), 跳过',
    '// 第4步：读第一个字节 —— JSON 以 { 开头，音频是二进制乱码': '// 第4步: 读第一个字节 —— JSON 以 { 开头, 音频是二进制乱码',
    '// 第5步：读完整个 JSON 响应体': '// 第5步: 读完整个 JSON 响应体',
    '// 第6步：确认包含 multi_artist 字段后，返回JSON给AI': '// 第6步: 确认包含 multi_artist 字段后, 返回JSON给AI',

    # === Audio puppet ===
    'ƵԾȣ300ms  = ˵': '音频弹跳检测: 300ms 不活跃 = 不说话',
    '˵50-200ms': '说话50-200ms脉冲',
    '˵ʱ': '不说话时不变',

    # === MP3 player ===
    'assets ȡ dog_bark.wav (16kHz mono s16)': '从 assets 分区读取 dog_bark.wav (16kHz mono s16)',
    'ѹ PCM ص Mp3PlayerDecodeSingleFile ѭԶ': '压缩 PCM 数据到 Mp3Player的DecodeSingleFile 循环自动释放',
    '输出到控制ִ̨б\u0169\ue000̶ Core 1ӿغģ': '输出到控制台',
}

# For any phrase not in M, try to match with a substring approach
with open(out, 'w', encoding='utf-8') as f:
    matched = 0
    unmatched = []
    for ph in sorted(phrases, key=lambda x: -len(x)):
        if ph in M:
            f.write(ph + '\t' + M[ph] + '\n')
            matched += 1
        else:
            unmatched.append(ph)
    
    if unmatched:
        f.write('\n# === UNMATCHED ===\n')
        for ph in unmatched:
            f.write('# ' + ph + '\n')

print(f'Mapped {matched}/{len(phrases)} phrases')
print(f'Unmatched: {len(unmatched)}')
if unmatched:
    with open(r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\_garb_unmatched.txt', 'w', encoding='utf-8') as f:
        for ph in unmatched:
            f.write(ph + '\n')
    print('Written unmatched to _garb_unmatched.txt')
print('Map file written!')
