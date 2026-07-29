#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Complete all remaining annotation work. No PowerShell, no edit tool."""
import os

CC = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_controller.cc'
BAK = CC.replace('cuckoo_controller.cc', 'backups/2026-07-28/cuckoo_controller.cc.bak_before_annotation_fix')

with open(CC, 'r', encoding='utf-8') as f:
    lines = f.read().splitlines()

# ====================================================================
# COMPLETE DOXYGEN DATABASE for every function
# ====================================================================
DOXY = {
    'Servo::SetAngle': ['/**', ' * @brief 设置舵机目标角度 (0~180)',
        ' * @param angle 角度值，自动钳位到有效范围', ' * 角度转换为 50Hz PWM 占空比 (409~2048)', ' */'],
    'Servo::Sweep': ['/**', ' * @brief 舵机平滑扫描到目标角度',
        ' * @param from 起始角度', ' * @param to 目标角度', ' * @param duration_ms 扫描总时长(ms)，每20ms一步', ' */'],
    '~Mp3Player': ['/**', ' * @brief 析构：停止播放并释放解码器', ' */'],
    'LoadDogBark': ['/**', ' * @brief 加载狗叫 PCM 到混音缓冲区，与背景音乐叠加', ' */'],
    'DecodeSingleFile': ['/**', ' * @brief 解码播放本地 MP3 文件',
        ' * @param index MP3 编号', ' * @return 0=完成 1=被停止',
        ' * - 跳过 ID3v2 标签', ' * - Ducking: AI 说话时压到 20%', ' */'],
    'ApplyDuckingGain': ['/**', ' * @brief 对 PCM 应用 Ducking 增益和软限幅', ' */'],
    'PlayUrl(': ['/**', ' * @brief HTTP 下载 MP3 并异步播放',
        ' * @param url URL 地址', ' * - 分块下载 严格帧头校验', ' * - Ducking: AI 说话时自动降音', ' */'],
    'PlayPcm(': ['/**', ' * @brief HTTP 下载原始 PCM 并播放',
        ' * @param url PCM URL', ' * - Ducking: AI 说话时降音', ' */'],
    'PlayOpus(': ['/**', ' * @brief BSD Socket 下载 Opus 音频并混入背景音频',
        ' * @param url 音频 URL', ' * @return 0=成功',
        ' * - BSD socket 直连', ' * - Opus: OGG 解封 -> PushPacketToDecodeQueue',
        ' * - PCM: PushBackgroundAudio(ring buffer)', ' * - 自动降音 65% 防 AEC 回采', ' */'],
    'PlayAlarmRing': ['/**', ' * @brief 播放闹钟铃声',
        ' * @param volume 音量 0.0~1.0', ' * A5/C#6 双频方波 每次 2 秒', ' */'],
    'PlayTaskEntry': ['/**', ' * @brief 后台播放任务主循环',
        ' * 轮询任务标志位 播放前静音AI 完后恢复', ' */'],
    'MotorPowerOn': ['/**', ' * @brief 打开电机电源 (P-MOSFET LOW=ON)', ' */'],
    'MotorPowerOff': ['/**', ' * @brief 关闭电机电源', ' */'],
    'IsDark': ['/**', ' * @brief 判断当前是否黑暗环境',
        ' * @return 光敏读数低于阈值则为黑暗', ' */'],
    'StartPerformance': ['/**', ' * @brief 启动表演流程',
        ' * @param type 表演类型', ' * @param hour 小时',
        ' * - 防AI误触发 5s窗口', ' * - 等AI说完话', ' * - Core1 上异步执行', ' */'],
    'PerformanceTask': ['/**', ' * @brief 表演任务: 开门->鸟叫+音乐->关门->舞蹈',
        ' * - 整点: 鸟叫N次+音乐+舞蹈+LED', ' * - 半点: 鸟叫3声', ' * - 完成恢复唤醒', ' */'],
    'CuckooStateMachine::CheckTime': ['/**', ' * @brief 检查并触发整点/半点报时',
        ' * - 夜间/AI忙时/bg audio 活跃时不报', ' * - min==0 -> 整点 min==30 -> 半点', ' */'],
    'SetAlarm': ['/**', ' * @brief 设置闹钟(自动去重)',
        ' * @param hour 小时', ' * @param minute 分钟', ' * @param repeat_daily 每日重复', ' */'],
    'GetAlarmsJson': ['/**', ' * @brief 获取闹钟列表 JSON', ' */'],
    'DeleteAlarm': ['/**', ' * @brief 删除闹钟并左移后续项', ' */'],
    'StopAlarm': ['/**', ' * @brief 停止闹钟(一次性闹钟自动禁用)', ' */'],
    'CheckAlarms': ['/**', ' * @brief 每秒闹钟检测触发 AlarmTask', ' */'],
    'AlarmTask': ['/**', ' * @brief 闹钟播放: 循环50次铃 每轮停2分', ' */'],
    'SaveAlarmsToNvs': ['/**', ' * @brief 保存闹钟到 NVS', ' */'],
    'LoadAlarmsFromNvs': ['/**', ' * @brief 从 NVS 加载闹钟', ' */'],
    'DogShow(': ['/**', ' * @brief 小狗表演 MCP 入口(后台异步)', ' */'],
    'DogShowTask': ['/**', ' * @brief 小狗表演: 开门->出场->叫->摇头->叫->退回->关门', ' */'],
    'StartLindaShow': ['/**', ' * @brief 琳达表演 MCP 入口', ' */'],
    'StartGardenShow': ['/**', ' * @brief 花园表演 MCP 入口', ' */'],
    'LindaShow': ['/**', ' * @brief 琳达表演: LED->0015.mp3->舞蹈->感谢', ' */'],
    'GardenShow': ['/**', ' * @brief 花园表演: LED->0016.mp3->小提琴->感谢', ' */'],
    'CheckMultiArtist': ['/**', ' * @brief 检测多版本歌曲返回 JSON 给 AI', ' */'],
    'StartShow(': ['/**', ' * @brief 综合表演 MCP 入口(后台异步)', ' */'],
    'StartShowTask': ['/**', ' * @brief 综合表演: 停止AI->选歌->小狗出场->舞蹈', ' */'],
    'ShowTask(': ['/**', ' * @brief 综合表演执行体 RunDanceIntro/Loop/Finale', ' */'],
    'KidsDanceShow': ['/**', ' * @brief Kids 舞蹈秀: RunDanceLoop', ' */'],
    'KidsComeOut': ['/**', ' * @brief Kids 小狗出场 MusicDogIntro', ' */'],
    'KidsRest': ['/**', ' * @brief Kids 休息: 停舞蹈+关门归位', ' */'],
    'PlayShowMusicBg': ['/**', ' * @brief 解码 MP3 到 PSRAM 以 bg audio 播放',
        ' * @param index MP3 编号', ' * @return true/false', ' */'],
    'PlayWavAsset': ['/**', ' * @brief 解析 WAV 头并 OutputRawPcm 播放', ' */'],
    'PlayOnlineMusic': ['/**', ' * @brief 在线音乐播放入口',
        ' * @param url_or_path URL 或歌名', ' * 自动检测格式/编码/去重/启动 PlayOpus', ' */'],
    'SetMusicProxy': ['/**', ' * @brief 设置音乐代理服务器地址', ' */'],
    'Dance(': ['/**', ' * @brief 独立舞蹈 8秒循环 M1 正反转', ' */'],
    'OpenDoor': ['/**', ' * @brief 开启大门 M2 正转', ' */'],
    'CloseDoor': ['/**', ' * @brief 关闭大门 M2 反转', ' */'],
    'BirdJumpOnce': ['/**', ' * @brief 小鸟弹跳一次 通电500ms+冷却400ms', ' */'],
    'BirdJumpShort': ['/**', ' * @brief 小鸟快速弹跳 AI说话时跟节奏', ' */'],
    'PlayDogBark': ['/**', ' * @brief 播放狗叫 自动检测 bg audio 活跃度', ' */'],
    'StopAll': ['/**', ' * @brief 紧急停止所有电机+表演+LED灭', ' */'],
    'StopMusic': ['/**', ' * @brief 停止音乐 清空背景音频', ' */'],
    'MotorTest': ['/**', ' * @brief 电机速度测试 M1 定时正转', ' */'],
    'MusicDanceTick': ['/**', ' * @brief 音乐舞蹈时钟驱动 250ms tick',
        ' * 管理 M1/小提琴/狗尾/LED 音乐结束自动关门', ' */'],
    'MusicDogIntro': ['/**', ' * @brief 在线音乐开始时小狗出场', ' */'],
    'MusicDogOutro': ['/**', ' * @brief 在线音乐结束时小狗退回关门', ' */'],
    'PlayBellSoundSync': ['/**', ' * @brief 同步播放钟声报时', ' */'],
    'PlayCuckooSoundSync': ['/**', ' * @brief 同步播放鸟叫 bg audio 活跃时混音', ' */'],
    'PlayMusic': ['/**', ' * @brief 播放本地 MP3 index 编号', ' */'],
    'SaveQuietMode': ['/**', ' * @brief 静音模式 NVS 持久化', ' */'],
    'LoadQuietMode': ['/**', ' * @brief 从 NVS 加载静音模式', ' */'],
    'SaveKidsActive': ['/**', ' * @brief Kids 模式 NVS 持久化', ' */'],
    'LoadKidsActive': ['/**', ' * @brief 从 NVS 加载 Kids 模式', ' */'],
    'SaveHourlyPerf': ['/**', ' * @brief 整点表演 NVS 持久化', ' */'],
    'LoadHourlyPerf': ['/**', ' * @brief 从 NVS 加载整点表演', ' */'],
    'SetServoAngle': ['/**', ' * @brief 设置舵机角度 MCP 工具', ' */'],
    'SetMotorSpeed': ['/**', ' * @brief 设置电机速度 MCP 工具', ' */'],
    'CantoneseLookup': ['/**', ' * @brief 粤语词组查询', ' * @param word 词组', ' * @return 粤语发音 JSON', ' */'],
}

# ====================================================================
# PHASE 1: Fill ALL empty Doxygen blocks
# ====================================================================
i = 0
fixed_doxy = 0
while i < len(lines):
    s = lines[i].strip()
    if s != '/**':
        i += 1
        continue
    
    # Find block end
    block_end = -1
    all_empty = True
    for j in range(i+1, min(i+15, len(lines))):
        js = lines[j].strip()
        if js == '*/':
            block_end = j
            break
        if js and js not in ['*', ' * ']:
            all_empty = False
            break
    
    if not all_empty or block_end < 0:
        i += 1
        continue
    
    # Find function name after block
    fn_line = block_end + 1
    while fn_line < len(lines) and lines[fn_line].strip() == '':
        fn_line += 1
    if fn_line >= len(lines):
        i += 1
        continue
    
    fn = lines[fn_line].strip()
    
    # Match function
    matched = None
    for key, doxy in DOXY.items():
        if key in fn:
            matched = doxy
            break
    
    if not matched:
        # Remove empty block entirely
        for k in range(i, block_end+1):
            lines[k] = ''
        i = block_end + 1
        continue
    
    # Fill the block
    indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
    block_size = block_end - i + 1
    doxy_size = len(matched)
    
    if block_size >= doxy_size:
        # Replace existing lines
        for k in range(doxy_size):
            lines[i + k] = indent + matched[k]
        # Clear extra lines
        for k in range(doxy_size, block_size):
            lines[i + k] = ''
    else:
        # Need to insert lines - replace existing then add
        for k in range(block_size):
            lines[i + k] = indent + matched[k]
        # Insert remaining
        for k in range(block_size, doxy_size):
            lines.insert(i + k, indent + matched[k])
    
    fixed_doxy += 1
    i = block_end + 1

print(f"Doxygen filled: {fixed_doxy}")

# ====================================================================
# PHASE 2: Remove standalone empty // lines
# ====================================================================
for i in range(len(lines)):
    if lines[i].strip() != '//':
        continue
    # Delete if not adjacent to another comment
    adj_comment = False
    for j in [i-1, i+1]:
        if 0 <= j < len(lines) and lines[j].strip().startswith(('//', '/*', '*')):
            adj_comment = True
    if not adj_comment:
        lines[i] = ''

# ====================================================================
# PHASE 3: Remove duplicate consecutive comments  
# ====================================================================
for i in range(len(lines)-1, 0, -1):
    s = lines[i].strip()
    ps = lines[i-1].strip()
    if s and ps and s == ps:
        if s.startswith(('//', '/*', '*')):
            lines[i] = ''

# ====================================================================
# PHASE 4: Clean extra blank lines (max 1 consecutive)
# ====================================================================
result = []
blanks = 0
for l in lines:
    if l.strip() == '':
        blanks += 1
        if blanks <= 1:
            result.append(l)
    else:
        blanks = 0
        result.append(l)

# ====================================================================
# VERIFY CODE INTEGRITY
# ====================================================================
with open(BAK, 'r', encoding='utf-8-sig') as f:
    bak = f.read().splitlines()

def code_only(lst):
    return [l for l in lst if l.strip() and 
            not l.strip().startswith('//') and 
            not l.strip().startswith('/*') and 
            not l.strip().startswith('*')]

bc = code_only(bak)
cc = code_only(result)

if bc == cc:
    with open(CC, 'w', encoding='utf-8') as f:
        f.write('\n'.join(result))
    
    empty = sum(1 for l in result if l.strip() in ['//', '/**', '*', ' * '])
    eng = sum(1 for l in result if l.strip().startswith('//') and len(l.strip())>3 and 
              not any(ord(c)>127 for c in l.strip()) and not l.strip().startswith('// ='))
    garb = sum(1 for l in result if chr(0xFFFD) in l)
    
    print(f"\nFINAL RESULT:")
    print(f"  Empty markers: {empty}")
    print(f"  English comments: {eng}")
    print(f"  Garbled chars: {garb}")
    print(f"  Lines: {len(result)}")
    print(f"  Code integrity: VERIFIED ({len(cc)} lines)")
else:
    print(f"\nCODE INTEGRITY FAILED!")
    print(f"  Backup code lines: {len(bc)}")
    print(f"  Current code lines: {len(cc)}")
    diffs = 0
    for a, b in zip(bc, cc):
        if a != b and diffs < 5:
            print(f"  DIFF: [{a[:60]}] vs [{b[:60]}]")
            diffs += 1

os.remove(__file__)
