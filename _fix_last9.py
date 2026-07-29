"""Fix last 9 doxygen blocks by approximate line number."""
import os

d = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

fixes = [
    ('cuckoo_part1.cc', 350, '/**\n * @brief 通过原始BSD Socket播放OGG/Opus音频流\n * @param url Opus音频URL\n * @return 0=成功\n * - 使用BSD socket替代lwip esp_http_client, 更高效\n * - OGG容器解Opus帧 -> PushPacketToDecodeQueue(原始Opus)\n * - PCM路径: legacyPushBackgroundAudio(高级音频ring buffer)\n * - Ducking: AI说话时暂停数据, 说完恢复\n * - 自动降低到65%音量(AEC回声补偿较高)\n * - 网络不通时自动切换串口模式\n */\n'),
    ('cuckoo_part3.cc', 91, '/**\n * @brief 闹钟播放: 循环播放50次(每次2秒, 共100秒) 15%渐强到100%, 每次之间停顿2秒\n * 期间检查alarm_stopped_标志, 用户停止则退出\n */\n'),
    ('cuckoo_part3.cc', 146, '/**\n * @brief 小狗表演: MCP入口\n * Core 1后台创建DogShowTask()执行\n */\n'),
    ('cuckoo_part4.cc', 80, '/**\n * @brief 小鸟弹跳: 通电500ms + 等待400ms\n */\n'),
    ('cuckoo_part4.cc', 100, '/**\n * @brief 小鸟弹跳(AI说话时触发)\n * 随机50-200ms通电, 等待300-700ms\n */\n'),
    ('cuckoo_part5.cc', 403, '/**\n * @brief 设置舵机角度\n * @param servo_id 0=小提琴舵机, 1=狗尾巴舵机\n * @param angle 0~180度\n */\n'),
    ('cuckoo_part5.cc', 414, '/**\n * @brief 设置电机速度\n * @param motor_id 1=M1舞蹈电机, 2=小提琴电机, 3=小狗, 4=鸟门电机\n * @param speed -100~100 (正=正转)\n */\n'),
    ('cuckoo_part5.cc', 437, '/**\n * @brief 播放URL音乐\n * @param url_or_path URL或路径 "/pcm?q=周杰伦"\n * @return 0=成功, <0=失败\n * - AI对话中不占用CPU\n * - 自动URL编码中文和空格\n * - 缺少http前缀自动拼接代理地址\n * - 已在播放时停止旧歌再发新歌\n */\n'),
    ('cuckoo_part5.cc', 719, '/**\n * @brief 设置音乐代理地址\n * @param host IP或域名\n * @param port 端口\n */\n'),
]

for part, target_ln, replacement in fixes:
    path = os.path.join(d, part)
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    # Find the nearest doxygen block
    for i in range(max(0, target_ln-15), min(len(lines), target_ln+15)):
        if '/**' in lines[i]:
            j = i
            while j < len(lines) and '*/' not in lines[j]:
                j += 1
            if j < len(lines):
                # Replace this block
                lines[i:j+1] = [replacement]
                break
    
    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print(f'{part}: L{target_ln} fixed')

print('Done')
