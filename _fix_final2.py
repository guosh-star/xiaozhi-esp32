"""Last pass: build exact-match replacements from _garb_final.txt against actual file content."""
import os

PARTS_DIR = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'
PARTS = ['cuckoo_part1.cc', 'cuckoo_part2.cc', 'cuckoo_part3.cc', 'cuckoo_part4.cc', 'cuckoo_part5.cc']

# Read actual garbled text from files
garbled_exact = {}
for part in PARTS:
    path = os.path.join(PARTS_DIR, part)
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            if '//' in line:
                comment = line.split('//', 1)[1].strip()
                # Check if genuinely garbled
                for c in comment:
                    cp = ord(c)
                    if cp <= 127: continue
                    if 0x4e00 <= cp <= 0x9fff: continue
                    if 0x2000 <= cp <= 0x27bf: continue  # punctuation, arrows, etc
                    if 0x3000 <= cp <= 0x303f: continue
                    if 0xff00 <= cp <= 0xffef: continue
                    if c in '，。！？；：''""（）〔〕【】《》—…·～→←↑↓°±':
                        continue
                    garbled_exact[comment] = True
                    break

# Build replacement map for remaining garbled text
# Key: exact garbled text from file, Value: correct Chinese text
replacements = {}

for exact in garbled_exact:
    # Map based on known patterns - use longest match first
    fixes = {
        # Partially garbled phrases that need fixing character by character
        '恢复标志Ѵʼ⣨ EnableVoiceProcessing   ResetDecoder  TTS': '恢复唤醒词初始化( EnableVoiceProcessing + ResetDecoder + TTS)',
        '⊥ӻָᵼ»Ѵ listening ⊥̬ AbortSpeaking  audio_input': '直接恢复会导致唤醒词 listening 状态 AbortSpeaking 被 audio_input 覆盖',
        'AI󴥷豸մidle5 performanceAIڻʱԶcuckoo.performance': 'AI未触发/设备处于idle超过5分钟→自动performance(AI在后台时自动cuckoo.performance)',
        'ע⣺StartShow  AbortSpeaking ⊥̬ listening idle': '注意: StartShow 等待 AbortSpeaking 状态从 listening 变 idle',
        '恢复标志Ѵֵ豸idleӿزת': '恢复唤醒词等待设备idle后钟控接管',
        
        # Performance/Show related
        '赸У㱨ʱ PerformanceTask + ۺϱ ShowTask ã': '舞蹈/半点和整点报时 PerformanceTask + 综合表演 ShowTask 都在这里',
        '====== 㱨ʱ򻯰棨ްװֻƵ̣======': '====== 整点报时+简单表演(无包装, 只有音频) ======',
        '====== 㱨ʱ+小狗+һ ======': '====== 整点报时+小狗+小提琴 ======',
        '㱨ʱÿ == 0 ʱ NeedHourlyChime ظ': '整点报时(每 == 0 时, NeedHourlyChime 防重复)',
        '㱨ʱÿ == 30 ʱ NeedHalfHourlyChime ظ': '半点报时(每 == 30 时, NeedHalfHourlyChime 防重复)',
        '飨ÿһΣ': '混合报时(每激活一次)',
        '㱨ʱ3ν': '半点报时3次鸣叫',
        '㱨ʱ': '整点报时',
        '/㱨ʱ  Show AI': '整点/半点报时 + Show + AI 对话',
        '- /㱨ʱ': '- 整点/半点报时',
        
        # Show roles
        '- GardenShow: ԰Lindaݣ0016 + 小狗ٶ + LED˸': '- GardenShow: 花园(类似Linda)(0016 + 小提琴动作 + LED闪烁)',
        '- LindaShow: մ赸ݣ0015 + 赸 + LED˸': '- LindaShow: 琳达舞蹈表演(0015 + 舞蹈 + LED闪烁)',
        '- DogShow: 小狗ݣšܳ小狗ҡͷ˻ءţ': '- DogShow: 小狗表演(出大门→前进→摇尾→后退→进门)',
        'ʵʱ DogShowTask() ִУ Core 1 ̨': '实际在 DogShowTask() 中执行, Core 1 后台任务。',
        '԰ŵ Core 1 ̨': '所以放到 Core 1 后台任务。',
        'ɫݣ MCP ߵʵ֣': '角色表演: MCP 工具的底层实现',
        '첽ִУMCP ߵѭ߳ϣapp.Schedule': '异步执行(MCP 工具的循环逻辑冲突app.Schedule)',
        '重试循环ȴ AbortSpeaking/TTS-STOP': '回调中等待 AbortSpeaking/TTS-STOP',
        
        # Show CuckooCore
        '壩ͣٿʼ': '清理背景, 等待开始',
        '壨ͬʱڲ': '脉冲跳跃(中平同时内部计数)',
        'ռãֹظ': '占用标志, 防止重复启动',
        'ֱ AbortSpeaking TTS Ȼ': '直接 AbortSpeaking 中断 TTS 然后清理',
        'ָֽѴʺ': '结束后恢复唤醒词和对话',
        
        # Hardware
        'Դ P-MOSFET  (GPIO LOW=ON, HIGH=OFF)': '电源 P-MOSFET 开关 (GPIO LOW=ON, HIGH=OFF)',
        'Ĭ϶ϵ磬100K  5V': '默认断电, 100K 下拉电阻保持 5V 稳定',
        'ͣˮͷGPIO39ͻ': '先停水车(拉低GPIO39避免冲突)',
        'ͨ磬小狗񵯳': '反向通电, 小鸟弹出',
        'ϵ磬小狗': '反向断电, 小鸟归位',
        '300-700msȴʱ': '随机300-700ms冷却时间',
        'ֲʱLOW_POWER': '串口模式保持LOW_POWER',
        '// 第7步：解码并播放': '// 第7步: 解码并播放',
        '// 第1步：先停掉当前播放': '// 第1步: 先停掉当前播放',
        
        # Cuckoo task
        '- Ƶʱˢ£ÿ10sֹI2SԴر�': '- 音频定时刷新(每10s防止I2S自动关闭)',
        'ȡԭ + RTC': '读取原因 + RTC',
        'ટNTPͬʱÿtick_sec%86400У׼': '注意NTP同步时每tick_sec%86400校准',
        '=== ÿ（sub_tick % 4 == 0）===': '=== 每4个sub_tick(sub_tick % 4 == 0) ===',
        
        # Chime phases
        'Phase 3: ָAIƵ': 'Phase 3: 恢复AI音频',
        '恢复标志AIƵ': '恢复AI音频',
        
        # Ducking / Audio
        'AI ˵ TTS ͬʱУPlayOpus Զ duck  30%': '等待 AI 说完 TTS 同时(PlayOpus 自动 duck 到 30%)',
        'AI ˵/ȴ': '等待 AI 说完/等待完成',
        'AI ˵ݺͬʱУֵӿ': '等待 AI 说完(表演和音乐同时开始, 有打断控制)',
        '===  AI ˵ݺͬʱ ===': '=== 等待 AI 说完(表演和音乐同时开始) ===',
        '=== ͬʱ AIֵӿȻ ===': '=== 同时 AI 打断控制逻辑 ===',
        'AI ղŶзֹ Opus': '等待 AI 停止(播放队列中停止 Opus)',
        'AI æʱ򱳾Ƶвʱ': 'AI 正忙时背景音频无播放时间',
        'AIݺͬʱ': '等待 AI 停止(表演和音乐同时开始)',
        'drain ݣֹ ring buffer': '先 drain 旧数据, 防止 ring buffer 残留',
        '15% ǿ 100% (ǰ10)': '15% 渐强到 100% (前10帧)',
        
        # Water bird puppet
        'AI不说话时不变小狗滰Ծڱޱʱ': 'AI说话时小鸟会说话弹跳(在后台报时期间)',
        'AI不说话时不变小狗滰Ծģʽ': 'AI说话时小鸟会说话+弹跳模式',
        
        # Motor compensation
        'ڽǶȲ279/s40s31Ȧʵ⣩ֻ360ڵĲ': '自转角度补偿 279°/s, 40秒约31圈(实际更多), 只补 360° 内的偏移',
        'ڽǶȲ279/s': '自转角度补偿 279°/s',
        
        # Misc
        'һԽ뵽PSRAMOutputRawPcmظţMP3򿪵Ķ': '最后一声直接进入PSRAM→OutputRawPcm(播放队列, MP3已打开的输出)',
        '输出到控制台ִбݣ̶ Core 1ӿغģ': '后台执行表演, 固定 Core 1(钟控核心)',
        'ģʽ NVS 洢': '静音模式 NVS 存储',
        'ID3v2 ǩ': 'ID3v2 标签',
        'ո HTTP request line Ƿָ': '构建 HTTP request line + 必要头部',
        'ôַƴ': '使用字符串拼接',
        '2 ַ': '替换 2 字符',
        'ѡ?': '选曲?',
        'ɺɾ': '任务完成后删除自身',
        'ͣ赸': '停止舞蹈',
        'Źر': '鸟门关闭',
        'һٹ': '最后一声后关门',
        'ֲ': '音乐不让duck',
        'Ŵ': '鸟门打开',
        # Actually many of these short ones are in "//" comments - check context
    }
    
    for garb_text, correct in fixes.items():
        if garb_text in exact:
            replacements[garb_text] = correct

print(f'Built {len(replacements)} exact-match replacements')

# Apply to all parts
for part_name in PARTS:
    path = os.path.join(PARTS_DIR, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original = content
    changed = 0
    for old, new in sorted(replacements.items(), key=lambda x: -len(x[0])):
        if old in content:
            content = content.replace(old, new)
            changed += 1
    
    if content != original:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f'{part_name}: {changed} replacements')
    else:
        print(f'{part_name}: no changes')

print('Done!')
