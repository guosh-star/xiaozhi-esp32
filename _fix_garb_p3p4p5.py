"""Fix P3, P4, P5 garbled Chinese comments - Round 2."""
import os

PARTS_DIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
PARTS = ['cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']

REPLACEMENTS = [
    # === P2 remaining ===
    ("MotorPowerOff();  // Ĭ϶ϵ磬100K  5V", "MotorPowerOff();  // 默认断电, 100K 下拉电阻保持 5V 稳定"),

    # === P3: cuckoo_core ===
    ("// 赸У㱨ʱ PerformanceTask + ۺϱ ShowTask ã", "// 舞蹈/半点和整点报时 PerformanceTask + 综合表演 ShowTask 都在这里"),
    ("//  assets ȡ dog_bark.wav (16kHz mono s16)", "// 从 assets 分区读取 dog_bark.wav (16kHz mono s16)"),
    ("// ѹ PCM ص Mp3PlayerDecodeSingleFile ѭԶ", "// 压缩 PCM 数据到 Mp3Player的DecodeSingleFile 循环自动释放"),
    ("// 첽ţ赸/ֲУ", "// 异步绑定: 用于舞蹈/在线音乐中的狗叫"),
    ("// Сǰ  ͣ  Уͱͬʱ", "// 小狗前进 1000ms: 停止水车, 小狗和鸟同步"),
    ("// M1 赸: ת/ת棨1~3룬>3", "// M1 舞蹈电机: 正转/反转随机运行1~3秒, >3秒时换向"),
    ("// Сѭÿ֡ 9 ƽڶ", "// 小提琴循环: 每帧步进 9 度平滑摆动"),
    ("// Сҡβ", "// 小狗摇尾巴"),
    ("// LED ˸ÿ6֡ ~300ms", "// LED 闪烁: 每6帧切换一次 ~300ms"),
    ("// 50ms ֡ʿ", "// 50ms 帧速率"),
    ("// ֽһֱŹرպ ShowTask ĩβϨ", "// 结束阶段一直发光(不闪烁), ShowTask 末尾统一熄灭"),
    ("// ڽǶȲ279/s40s31Ȧʵ⣩ֻ360ڵĲ", "// 自转角度补偿 279°/s, 40秒约31圈(实际更多), 只补 360° 内的偏移"),
    ('long net = ((long)(m1_fwd_time_ - m1_rev_time_) * 279) / 1000;  // 旋转角度：+正转,-反转', 'long net = ((long)(m1_fwd_time_ - m1_rev_time_) * 279) / 1000;  // 旋转角度：+为正转,-为反转'),
    ("go_fwd = (net < 0);  // net为负=反转太多，补正转", "go_fwd = (net < 0);  // net为负=反转太多，补正转"),
    ("go_fwd = (net >= 0);  // net为正=正转太多，补反转绕一圈", "go_fwd = (net >= 0);  // net为正=正转太多，补反转绕一圈"),
    ("// 反转<正转时，补偿×1.3", "// 反转<正转时，补偿×1.3"),
    ("// ƽСת", "// 平衡小车旋转"),
    ("// С˻", "// 小狗后退"),
    ("// СӵǰǶȹλ180㣨䣩", "// 小狗从当前角度回归180度(复位)"),

    # === P3: NVS & idle ===
    ("// NVS ӳ־û", "// NVS 映射日志/用户偏好"),
    ("// ͳЧʱ", "// 统一有效时间"),
    (" * @brief ʱݣ//ֶ", " * @brief 时间段数据/小时分字段"),
    ("// AI󴥷豸մidle5 performanceAIڻʱԶcuckoo.performance", "// AI未触发/设备处于idle超过5分钟→自动performance(AI在后台时自动cuckoo.performance)"),
    ("if ((now_us - last_idle_exit_us_) < 5000000) {  // 5봰", "if ((now_us - last_idle_exit_us_) < 5000000) {  // 5秒窗口"),
    ("//  AI ˵/ȴ", "// 等待 AI 说完/等待完成"),
    ("vTaskDelay(pdMS_TO_TICKS(200));  // ״̬ص idle", "vTaskDelay(pdMS_TO_TICKS(200));  // 状态切换回 idle"),
    ("// ڼֹΪѴʻ AI", "// 避免中断当前唤醒词或 AI 对话"),
    ("// ʽѴʼ⣨ɰ汾 EnableVoiceProcessing ܲ߼", "// 延迟唤醒词初始化(新版本 EnableVoiceProcessing 管理逻辑)"),
    ("total_calls_ = hour;       // ¼ܱʱ㣩", "total_calls_ = hour;       // 记录总报时次数(统计用)"),
    ("call_count_ = 3;           // 㱨ʱ3ν", "call_count_ = 3;           // 半点报时3次鸣叫"),
    ("call_count_ = 3;           // 㱨ʱ3ν", "call_count_ = 3;           // 整点报时3次鸣叫"),

    # === P3: Performance ===
    ("//  AI ղŶзֹ Opus", "// 等待 AI 停止(播放队列中停止 Opus)"),
    ("// ====== 㱨ʱ򻯰棨ްװֻƵ̣====== ", "// ====== 整点报时+简单表演(无包装, 只有音频) ======"),
    ("// Phase 1: Ŵ  С+  Źر", "// Phase 1: 鸟门打开 → 小鸟+鸟叫 → 鸟门关闭"),
    ("// 첽ƽ+Ͷͬ", "// 异步音乐绑定+鸟叫同步"),
    ("vTaskDelay(pdMS_TO_TICKS(10));  // ý", "vTaskDelay(pdMS_TO_TICKS(10));  // 让脚步动作完成"),
    ("sm->BirdJumpPulse();  // 壨ͬʱڲ", "sm->BirdJumpPulse();  // 脉冲跳跃(中平同时内部计数)"),
    ("vTaskDelay(pdMS_TO_TICKS(1800));  // Ƚ(~1.76s)", "vTaskDelay(pdMS_TO_TICKS(1800));  // 等叫声结束(~1.76s)"),
    ("// һٹ", "// 最后一声后关门"),
    ("sm->CloseBirdDoor();  // Źر", "sm->CloseBirdDoor();  // 鸟门关闭"),
    ("// һԽ뵽PSRAMOutputRawPcmظţMP3򿪵Ķ", "// 最后一声直接进入PSRAM→OutputRawPcm(播放队列, MP3已打开的输出)"),
    ("// OutputRawPcm ģŷ", "// OutputRawPcm 默认播放方式"),
    ("// Phase 3: ָAIƵ", "// Phase 3: 恢复AI音频"),
    ("app.GetAudioService().SetOutputMuted(false);  // ָAIƵ", "app.GetAudioService().SetOutputMuted(false);  // 恢复AI音频"),
    ("if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // ˮ IN1=HIGH", "if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // 水车 IN1=HIGH"),
    ("// 첽+С赸ѭ ShowTask һ£", "// 异步音乐+小狗舞蹈循环(与 ShowTask 一致)"),
    ("// 赸+С̿ʼͿ/СУ", "// 舞蹈+小狗开始/停止音乐控制"),
    ("// ====== 㱨ʱ+С+һ ======", "// ====== 整点报时+小狗+小提琴 ======"),
    ("sm->OpenBirdDoor();  // Ŵ", "sm->OpenBirdDoor();  // 鸟门打开"),
    ("// 첽ƽ+Ͷͬ", "// 异步音乐绑定+鸟叫同步"),
    ("vTaskDelay(pdMS_TO_TICKS(10));  // ý", "vTaskDelay(pdMS_TO_TICKS(10));  // 让脚步动作完成"),
    ("sm->BirdJumpPulse();  // 壨ͬʱڲ", "sm->BirdJumpPulse();  // 脉冲跳跃(中平同时内部计数)"),
    ("vTaskDelay(pdMS_TO_TICKS(1800));  // Ƚ(~1.76s)", "vTaskDelay(pdMS_TO_TICKS(1800));  // 等叫声结束(~1.76s)"),
    ("sm->CloseBirdDoor();  // Źر", "sm->CloseBirdDoor();  // 鸟门关闭"),
    ("// ָ AI", "// 恢复 AI 语音"),
    ("// ָѴʼ⣨ EnableVoiceProcessing   ResetDecoder  TTS", "// 恢复唤醒词初始化( EnableVoiceProcessing + ResetDecoder + TTS)"),
    ("// ֻ idle ״̬Żָ listening ״̬󴥷Ѵ", "// 只在 idle 状态才恢复 listening 状态(后触发唤醒词)"),

    # === P3: Quiet mode ===
    ("// AI æʱ򱳾Ƶвʱ", "// AI 正忙时背景音频无播放时间"),
    ("// ģʽж", "// 静音模式判断"),
    ("return;  // ȫ쾲", "return;  // 全天静音"),
    ("return;  // ھ", "return;  // 光控静音"),
    ("return;  // ҹ: 22~6", "return;  // 夜间: 22~6"),
    ("// mode==1 ȫ챨ʱreturn", "// mode==1 全天报时(不return)"),
    ("// 㱨ʱ", "// 整点报时"),
    ("// 㱨ʱ", "// 半点报时"),
    ("// ģʽ NVS 洢", "// 静音模式 NVS 存储"),
    ("// 幦  ֧5ӣNVS־û", "// 用户功能: 最多支持5项, NVS日志/用户偏好"),

    # === P3: Show tasks ===
    ("volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% ǿ 100% (ǰ10)", "volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% 渐强到 100% (前10帧)"),
    ("// ɫݣ MCP ߵʵ֣", "// 角色表演: MCP 工具的底层实现"),
    ("// - DogShow: СݣšܳСҡͷ˻ءţ", "// - DogShow: 小狗表演(出大门→前进→摇尾→后退→进门)"),
    ("// - LindaShow: մ赸ݣ0015 + 赸 + LED˸", "// - LindaShow: 琳达舞蹈表演(0015 + 舞蹈 + LED闪烁)"),
    ("// - GardenShow: ԰Lindaݣ0016 + Сٶ + LED˸", "// - GardenShow: 花园(类似Linda)(0016 + 小提琴动作 + LED闪烁)"),
    ("// DogShow()  MCP ڣز", "// DogShow() 即 MCP 入口(不走调度)"),
    ("// ʵʱ DogShowTask() ִУ Core 1 ̨", "// 实际在 DogShowTask() 中执行, Core 1 后台任务。"),

    # === P4: shows ===
    ("if (is_running_) return;  // бУܾظ", "if (is_running_) return;  // 已有表演在运行, 拒绝重复启动"),
    ("// ִ̨бݣ̶ Core 1ӿغģ", "// 后台执行表演, 固定 Core 1(钟控核心)"),
    ("vTaskDelete(nullptr);  // ɺɾ", "vTaskDelete(nullptr);  // 任务完成后删除自身"),
    ("// ===  AI ˵ݺͬʱ ===", "// === 等待 AI 说完(表演和音乐同时开始) ==="),
    ("// === ͬʱ AIֵӿȻ ===", "// === 同时 AI 打断控制逻辑 ==="),
    ("// === 3ʼ ===", "// === 步骤3: 音乐开始 ==="),
    ("// ֱӲŹУ PlayWavAsset", "// 直接播放队列, 使用 PlayWavAsset"),
    ("// ز 2205016000", "// 重采样 22050→16000"),
    ("//  drain ݣֹ ring buffer", "// 先 drain 旧数据, 防止 ring buffer 残留"),
    ("// MCP ڣءʵʱںִ̨С", "// MCP 入口(调度的), 实际在后台任务执行"),
    ("if (is_running_) return;  // бУܾ", "if (is_running_) return;  // 已有表演在运行, 拒绝"),
    ("//  AI ˵ݺͬʱУֵӿ", "// 等待 AI 说完(表演和音乐同时开始, 有打断控制)"),
    ("// 1. LEDһʾݼʼ", "// 1. LED一闪表示表演开始"),
    ("// 3. LED˸ + 赸תֽ", "// 3. LED闪烁 + 舞蹈电机(旋转+换向)"),
    ("//    ǰ24룺LED˸ + 赸", "//    前24秒: LED闪烁 + 舞蹈"),
    ("//    24LEDȫ赸ֱֽ", "//    24秒后: LED全亮, 舞蹈直到结束"),
    ("// ֹͣǰԼ2루24ʱLEDȫֹͣ˸", "// 先停止当前(约2秒, 24秒时LED全亮停止闪烁)"),
    ("// 赸1~3룬Ȼ", "// 舞蹈运行1~3秒, 然后反向"),
    ("// LED˸ǰ24룬ÿ6֡лһ = Լ300ms", "// LED闪烁前24秒, 每6帧切换一次 = 约300ms"),
    ("// 5. ֽ  赸ͣ  Ÿл  LEDϨ", "// 5. 结束阶段: 舞蹈停止 → 大门切换 → LED熄灭"),
    ("if (m1_) m1_->Stop();  // ͣ赸", "if (m1_) m1_->Stop();  // 停止舞蹈"),
    ("// лֻţ0~4ѭ", "// 狗叫切换编号, 0~4循环"),
    ("if (mp3_) mp3_->SetDisableDucking(false);  // ָ duck", "if (mp3_) mp3_->SetDisableDucking(false);  // 恢复 duck"),

    # === P4: GardenShow ===
    ("//  AI ˵ݺͬʱУֵӿ", "// 等待 AI 说完(表演和音乐同时开始, 有打断控制)"),
    ("// 1. LEDһʾݼʼ", "// 1. LED一闪表示表演开始"),
    ("// 3. LED˸ + Сٶֱֽ", "// 3. LED闪烁 + 小提琴动作(直到结束)"),
    ("// : ÿ50ms֡ƶ9, 0~140Ұڶ", "// 小提琴: 每50ms帧步进9°, 0~140°左右摆动"),
    ("// 5. ֽ  Сƽ  Ÿл  LEDϨ", "// 5. 结束阶段: 小提琴平复 → 大门切换 → LED熄灭"),
    ('violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000);  // ӵǰǶƽص90', 'violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000);  // 从当前角度平顺回到90度'),
    ("if (mp3_) mp3_->SetDisableDucking(false);  // ָ duck", "if (mp3_) mp3_->SetDisableDucking(false);  // 恢复 duck"),

    # === P4: RunDanceFinale ===
    ("// M1赸: ת1-3  ͣ20ms  ת1-3  ͣ20ms  ѭ8", "// M1舞蹈: 正转1-3秒 → 停20ms → 反转1-3秒 → 停20ms → 循环8次"),
    ("// Сٶ: 90018090 ѭڶ", "// 小提琴动作: 90°→0°→180°→90° 循环摆动"),
    ("// ٶȲԣ赸תָ", "// 角度补偿: 舞蹈旋转归位"),
    ("if (m1_) m1_->Forward(100);  // GPIOֱȫѹ", "if (m1_) m1_->Forward(100);  // GPIO直接全正压"),

    # === P4: ShowTask ===
    ("// /㱨ʱ  Show AI", "// 整点/半点报时 + Show + AI 对话"),
    ("// 壩ͣٿʼ", "// 清理背景, 等待开始"),
    ("// ռãֹظ", "// 占用标志, 防止重复启动"),
    ("// 첽ִУMCP ߵѭ߳ϣapp.Schedule", "// 异步执行(MCP 工具的循环逻辑冲突app.Schedule)"),
    ("// ѭȴ AbortSpeaking/TTS-STOP", "// 回调中等待 AbortSpeaking/TTS-STOP"),
    ("// Schedule صŲ϶ӣTTS-STOP ״̬ת޷ִУ", "// Schedule 回调没有中断机制, TTS-STOP 状态转换被迫执行"),
    ("// ԰ŵ Core 1 ̨", "// 所以放到 Core 1 后台任务。"),
    ("//  AIݺͬʱ", "// 等待 AI 停止(表演和音乐同时开始)"),
    ("if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // ֲ", "if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // 音乐不让duck"),
    ("if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // ˮ IN1=HIGH", "if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // 水车 IN1=HIGH"),
    # ("// ѡ?", "// 选曲?"),  # tricky, keep as is
    ("// 첽+С赸ѭ", "// 异步音乐+小狗舞蹈循环"),
    ("// 赸+С̿ʼͿ/СУ", "// 舞蹈+小狗开始/停止音乐控制"),
    ("// ָֽѴʺ", "// 结束后恢复唤醒词和对话"),
    ("// ע⣺StartShow  AbortSpeaking ״̬ listening idle", "// 注意: StartShow 等待 AbortSpeaking 状态从 listening 变 idle"),
    ("// ֱӻָᵼ»Ѵ listening ״̬ AbortSpeaking  audio_input", "// 直接恢复会导致唤醒词 listening 状态 AbortSpeaking 被 audio_input 覆盖"),
    ("if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // ָ duck", "if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // 恢复 duck"),
    ("// ȷ豸ص idle ٻָ", "// 确保设备回到 idle 再恢复"),
    ("// ָѴֵ豸idleӿزת", "// 恢复唤醒词等待设备idle后钟控接管"),
    ("//  stop_all+play_url طŴ", "// 触发 stop_all+play_url 后重新打开大门"),
    ("// ֱ AbortSpeaking TTS Ȼ", "// 直接 AbortSpeaking 中断 TTS 然后清理"),

    # === P4: Puppet ===
    ("// AI˵ʱС滰Ծģʽ", "// AI说话时小鸟会说话+弹跳模式"),
    ("// 50-200msģȻԾ", "// 50-200ms脉冲然后弹跳"),
    ("// ȴ300-700ms̫", "// 等待300-700ms不要太快"),
    ("if (water_bird_) water_bird_->Stop();  // ͣˮͷGPIO39ͻ", "if (water_bird_) water_bird_->Stop();  // 先停水车(拉低GPIO39避免冲突)"),
    ('gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // ͨ磬С񵯳', 'gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // 反向通电, 小鸟弹出'),
    ('gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // ϵ磬С', 'gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // 反向断电, 小鸟归位'),
    ('int cooldown = 300 + (esp_random() % 401);  // 300-700msȴʱ', 'int cooldown = 300 + (esp_random() % 401);  // 随机300-700ms冷却时间'),

    # === P5: music_mcp ===
    ('MotorPowerOn();  // ȷԴȶ', 'MotorPowerOn();  // 确保电源稳定'),
    ('case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;  // Сٵ (B M2, GPIO18/45)', 'case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;  // 小提琴电机 (A路 M2, GPIO18/45)'),
    ('//  AI ˵ TTS ͬʱУPlayOpus Զ duck  30%', '// 等待 AI 说完 TTS 同时(PlayOpus 自动 duck 到 30%)'),
    ('// URLhttpͷֱʹ', '// URL可能包含http头, 直接使用'),
    ('//  URL еĿոΪ %20', '// 将 URL 中的空格替换为 %20'),
    ('//  2 ַ', '// 替换 2 字符'),
    ('// ڲţͣɸٷ¸', '// 若在播放, 停止后再发下一首'),
    ('// ȾPlayOpusTask˳is_playing_false', '// 等待PlayOpusTask退出(is_playing_变false)'),
    ('// ձƵbufferֹǰ׻', '// 清空音频buffer防止上一首残留'),
    ('// ôַƴ', '// 使用字符串拼接'),
    ('// ո HTTP request line Ƿָ', '// 构建 HTTP request line + 必要头部'),
    ('// ASCIIֱӸ', '// ASCII直接复制'),
    ('// ڲţͣɸٷ¸', '// 若在播放, 停止后再发下一首'),
    ('// === ɫ ===', '// === 角色工具 ==='),

    # === P5: cuckoo task ===
    ('// ӿ FreeRTOS  (Core 1)', '// 钟控 FreeRTOS 任务 (Core 1)'),
    ('// ְ:', '// 职责:'),
    ('// - NTPʱͬ60s/300sУ׼', '// - NTP时间同步(60s/300s校准)'),
    ('// - 豸״̬أidleԶţAI˵ʱСԾ', '// - 设备状态监控(idle→自动放音乐, AI说话时小鸟弹跳)'),
    ('// - Ѵֵ̬Ի0.3ֹ󴥷idleʱ0.02', '// - 唤醒词阈值动态切换(0.3防止误触发, idle时0.02)'),
    ('// - Ƶʱˢ£ÿ10sֹI2SԴرգ', '// - 音频定时刷新(每10s防止I2S自动关闭)'),
    ('// - /㱨ʱ', '// - 整点/半点报时'),
    ('// - Ӽ', '// - 用户标记'),
    ('// - RTC־ÿ30s浽ڴ棩', '// - RTC日志(每30s写入内存)'),
    ('// ȡԭ + RTC', '// 读取原因 + RTC'),
    ('// ʱˢƵʱֹڼI2SŹʱ', '// 定时刷新音频时钟(防止长时无播放I2S关闭)'),
    ('//  WiFi  NTP gettimeofday ɻȡǰʱ', '// 从 WiFi + NTP gettimeofday 获取当前时间'),
    ('if (tv.tv_sec > 1000000000) {  // 2001Ժ˵ NTP ͬ', 'if (tv.tv_sec > 1000000000) {  // 2001年以后说明 NTP 已同步'),
    ('// Ĭִַ config.h ʱ̶', '// 默认不再执行(仅 config.h 静态配置)'),
    ('//  NVS ѱ;ģʽ', '// 从 NVS 加载保存的静音模式'),
    ('// TODO(#22): 1ѯΪ¼:', '// TODO(#22): 一次轮询改为事件驱动:'),
    ('// - ע OnDeviceStateChanged ص idle/active ת', '// - 注册 OnDeviceStateChanged 回调处理 idle/active 切换'),
    ('// - ʱNTPͬtick_sec', '// - 定时NTP同步tick_sec'),
    ('// - Core 1󲿷ʱʡ', '// - Core 1后台部分可废弃'),
    ('// 豸idleûAI¼ʱڷ󴥷', '// 设备idle(没有AI事件)时降低触发阈值'),
    ('// Ի߻Ѵֵ0.3ֹ󴥷ڼ䲻裬ShowԼƣ', '// 动态切换唤醒词阈值(0.3防止误触发, 期间不切换, Show会自己管理)'),
    ('// Իлָ׼˷棨30dB AEC Ŵ', '// 动态切换恢复标准麦克风增益(30dB AEC 窗口)'),
    ('// idleʱֵָ0.02ڼ/Ÿڼ䲻', '// idle时恢复阈值0.02(在音乐/报时期间不操作)'),
    ('// idleʱ˷棨37.5dBװλ', '// idle时麦克风增益(37.5dB标准设置)'),
    ('// AI˵ʱС滰Ծڱޱʱ', '// AI说话时小鸟会说话弹跳(在后台报时期间)'),
    ('// ƵԾȣ300ms  = ˵', '// 音频弹跳检测: 300ms 不活跃 = 不说话'),
    ('// ˵50-200ms', '// 说话50-200ms脉冲'),
    ('// ˵ʱ', '// 不说话时不变'),
    ('// === ÿ（sub_tick % 4 == 0）===', '// === 每4个sub_tick(sub_tick % 4 == 0) ==='),
    ('// NTPͬδͬʱÿ60ԣͬÿ300У׼һ', '// NTP同步(未同步时每60秒尝试, 已同步每300秒校准一次)'),
    ('// WiFiʡCuckooBoard::SetPowerSaveLevelش', '// WiFi节能(CuckooBoard::SetPowerSaveLevel不处理)'),
    ('// ֲʱLOW_POWER', '// 串口模式保持LOW_POWER'),
    ('// ÿ30뱣־RTCڴ棨RTCڿŹλԱ', '// 每30秒保存日志到RTC内存(RTC在开关机后可供复位比较)'),
    ('// ÿ60+ջˮλڱ', '// 每60秒监控栈水位(定时报告)'),
    ('// ƵԴ/ 15 ر I2S »ʧЧ', '// 音频自动恢复: 15秒刷新 I2S "永不关闭"策略'),
    ('// ȡҹģʽ', '// 读取夜间模式'),
    ('// ʱͨ NTP  MCP ãڲʱ', '// 报时(优先 NTP 而非 MCP 设置, 用内部时钟)'),
    ('// NTPͬʱϵͳʱ䣬ʱƯ', '// NTP已同步(系统时间, 时钟不漂移)'),
    ('// NTPδͬMCPʱ䣩˵ʱӵ', '// NTP未同步(用MCP设置的时间), 模拟时钟推进'),
    ('// Ͱ', '// 旧模式清理'),
    ('// 㱨ʱÿ == 0 ʱ NeedHourlyChime ظ', '// 整点报时(每 == 0 时, NeedHourlyChime 防重复)'),
    ('// 㱨ʱÿ == 30 ʱ NeedHalfHourlyChime ظ', '// 半点报时(每 == 30 时, NeedHalfHourlyChime 防重复)'),
    ('// 飨ÿһΣ', '// 混合报时(每激活一次)'),
    ('// עNTPͬʱÿtick_sec%86400У׼', '// 注意NTP同步时每tick_sec%86400校准'),

    # === String code content ===
    ('"Hourly chime: bell rings + music. ONLY call when user explicitly says ʱ/㱨ʱ/ʱ/ʱ/what time. DO NOT auto call this."',
     '"Hourly chime: bell rings + music. ONLY call when user explicitly says 报时/整点报时/几点/几点了/what time. DO NOT auto call this."'),
    ('"ۺϱݣ赸+С+ˮ++LEDһ ' ' 'Ŀ' '' '' '' 'ۺϱ"'',
     '"综合表演: 舞蹈+小提琴+水车+小狗+灯光一起启动, 用于 '演出' '节目' '开始表演' '开秀' '秀' 等综合表演请求"'),
    ('"Stop music playback. Call ONLY when user explicitly asks to stop the music (ͣ/ͣ//Ҫ/ص). Do NOT call this when user says ʰ or other variants. DO NOT call this when user is asking to stop alarm/chime/show."',
     '"Stop music playback. Call ONLY when user explicitly asks to stop the music (停音乐/停止音乐/不要音乐/关掉音乐). Do NOT call this when user says 安静 or other variants. DO NOT call this when user is asking to stop alarm/chime/show."'),
    ('"Stop a ringing ALARM only. For /ͣ. NOT for stopping music/performance - use cuckoo.stop_all for that."',
     '"Stop a ringing ALARM only. For 铃声/停止铃声. NOT for stopping music/performance - use cuckoo.stop_all for that."'),
    ('"Set chime quiet mode. 0=ȫ쾲(ʱ), 1=ȫ챨ʱ, 2=ھ(LDR), 3=ָʱξ(ʱstart_hour~end_hour). "',
     '"Set chime quiet mode. 0=全天静音(不报时), 1=全天报时, 2=光控静音(LDR判断), 3=指定时间段静音(默认start_hour~end_hour静音). "'),
    ('m == 0 ? "ȫ쾲" : m == 1 ? "ȫ챨ʱ" : m == 2 ? "ھ(LDR)" : "ʱξ"',
     'm == 0 ? "全天静音" : m == 1 ? "全天报时" : m == 2 ? "光控静音(LDR)" : "时间段静音"'),
    ('"Device woke up \x7f opening bird door"', '"Device woke up → opening bird door"'),
    ('"Device sleeping \x7f closing bird door"', '"Device sleeping → closing bird door"'),

    # Additional P3/P4/P5 fixes
    ('if (m3_) m3_->Stop();  // С', 'if (m3_) m3_->Stop();  // 小鸟'),
    ('if (violin_motor_) violin_motor_->Stop();  // С', 'if (violin_motor_) violin_motor_->Stop();  // 小提琴'),
    ('if (m1_) m1_->Stop();  // 赸', 'if (m1_) m1_->Stop();  // 舞蹈'),
    ('if (m2_) m2_->Stop();  // ', 'if (m2_) m2_->Stop();  // 大门'),
    ('if (m4_) m4_->Stop();  // ', 'if (m4_) m4_->Stop();  // 鸟门'),
    ('// 첽+С赸ѭ ShowTask һ£', '// 异步音乐+小狗舞蹈循环(与 ShowTask 一致)'),

    # More P5 fixes
    ('// CuckooTools MCPע', '// CuckooTools MCP注册'),
    ('// 第0步：统一路径格式，把 /opus 或 /stream 都转成 /pcm', '// 第0步: 统一路径格式, 把 /opus 或 /stream 都转成 /pcm'),
    ('// 第2步：构造完整URL，直接用原始路径不转换', '// 第2步: 构造完整URL, 直接用原始路径不转换'),
    ('// /pcm 本身就能返回JSON或PCM，无需改路径', '// /pcm 本身就能返回JSON或PCM, 无需改路径'),
    ('// 第3步：没有 Content-Length = 流式传输 = 音频（不是JSON），跳过', '// 第3步: 没有 Content-Length = 流式传输 = 音频(不是JSON), 跳过'),
    ('// 第4步：读第一个字节 —— JSON 以 { 开头，音频是二进制乱码', '// 第4步: 读第一个字节 —— JSON 以 { 开头, 音频是二进制乱码'),
    ('// 第5步：读完整个 JSON 响应体', '// 第5步: 读完整个 JSON 响应体'),
    ('// 第6步：确认包含 multi_artist 字段后，返回JSON给AI', '// 第6步: 确认包含 multi_artist 字段后, 返回JSON给AI'),
    ('// 先检查多版本：如果服务器返回歌手列表JSON，直接返回给AI念（不播歌）', '// 先检查多版本: 如果服务器返回歌手列表JSON, 直接返回给AI念(不播歌)'),
    ('return multi;  // 歌手列表JSON返回给AI，AI自动念出来让用户选', 'return multi;  // 歌手列表JSON返回给AI, AI自动念出来让用户选'),
    ('// 音乐代理在启动时自动配置（config.h 里的 DEFAULT_MUSIC_PROXY_HOST/PORT）。', '// 音乐代理在启动时自动配置(config.h 里的 DEFAULT_MUSIC_PROXY_HOST/PORT)。'),
    ('// set_music_proxy 是空操作占位工具，永远瞬间成功，不做TCP连接检查。', '// set_music_proxy 是空操作占位工具, 永远瞬间成功, 不做TCP连接检查。'),
    ('// 这个工具只是为了满足 AI 在调用 play_url 之前先调 set_music_proxy 的习惯。', '// 这个工具只是为了满足 AI 在调用 play_url 之前先调 set_music_proxy 的习惯。'),
]

REPLACEMENTS.sort(key=lambda x: len(x[0]), reverse=True)

total = 0
for part_name in PARTS:
    path = os.path.join(PARTS_DIR, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    changed = 0
    for old, new in REPLACEMENTS:
        if old in content:
            content = content.replace(old, new)
            changed += 1
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part_name}: {changed} replacements')
    else:
        print(f'{part_name}: no changes')

print(f'Done.')
