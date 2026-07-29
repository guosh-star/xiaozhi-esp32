#!/usr/bin/env python
"""Properly split cuckoo_controller.cc into 5 independent parts.
Each part has its own #include header block. No methods lost."""
import os, re

BASE = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32'
SRC = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock', 'cuckoo_controller.cc')
DST = os.path.join(BASE, 'main', 'boards', 'cuckoo-clock')

# Read source
with open(SRC, 'r', encoding='utf-8-sig') as f:
    lines = f.readlines()
print(f'Source: {len(lines)} lines')

# Define split boundaries (0-indexed)
# P1: L1-L954  - Includes, Motor, Servo, Mp3Player, PlayPcmTask
# P2: L955-L2046 - PlayOpusTask, LdrSensor, BellSoundPlayer, CuckooStateMachine constructor
# P3: L2047-L2954 - MotorPowerOn, Performance/Chime core, DogShow
# P4: L2955-L3893 - DogShowTask, LindaShow, GardenShow, Dance, ShowTask, StopAll
# P5: L3894-L5051 - MusicDanceTick, Kids, MCP tools, cuckoo_clock_task

parts = {
    'cuckoo_part1.cc': (0, 954),
    'cuckoo_part2.cc': (954, 2046),
    'cuckoo_part3.cc': (2046, 2954),
    'cuckoo_part4.cc': (2954, 3893),
    'cuckoo_part5.cc': (3893, len(lines)),
}

# The header block (includes + defines) - same for all parts
HEADER = '''// ============================================
// 布谷鸟钟 控制器 (Cuckoo Controller)
//
// 功能概述
// - 4路直流电机 + 1路步进电机 + 水车
// - 4个舵机(鸟门/小狗/小提琴/狗尾巴) + 2个普通舵机
// - LED 灯2路 LED_A / LED_B
// - 音频播放: MP3/Opus/PCM 解码 + HTTP 流下载 + AI Ducking 混音控制
// - 布谷鸟/整点报时: 开门 + 小鸟跳跃 + 鸟叫 + 转圈
// - 综合表演: 舞蹈 + 小提琴 + 水车 + 小狗 + LED
// - 角色 MCP 工具: DogShow(小狗), LindaShow(琳达), GardenShow(花园)
// - 用户功能: NVS 日志/用户偏好5项/每日重复
// - 夜间模式: 22:00-6:00 禁止报时
// - 在线音乐: QQ音乐代理/Opus/PCM 格式解码推送
// - MCP 注册(21个工具), 通过 MCP 协议供 AI 大模型调用
//
// 线程架构
// - Core 0: 语音(唤醒词/TTS/Opus 解码)
// - Core 1: 钟控(cuckoo_clock_task/250ms tick/定时和报时)
// - 音乐/报时通过 xTaskCreatePinnedToCore 到 Core 1 执行
// ============================================

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "cuckoo_controller.h"
#include "assets.h"
#include "server/constants.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_spiffs.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <lwip/sockets.h>

#include <cJSON.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cmath>

#define TAG "CuckooCtrl"

'''

# Forward declarations for P5
P5_FORWARD = '''
// Forward declarations for types/functions defined in P1-P2
struct DoorOpenCtx {
    class CuckooStateMachine* sm;
};
void ConvertToPcmUrl(char* url, size_t url_sz);
// RTC crash log struct (defined in P1)
struct RtcCrashLog {
    uint32_t magic;
    uint32_t tick_sec;
    uint8_t  dev_state;
    uint8_t  music_active;
    uint32_t free_heap;
};
extern RtcCrashLog rtc_crash_log;

'''

# Also need to add forward declarations for BellSoundPlayer methods used in P5
P5_EXTRA_INCLUDES = '''
// Additional includes needed for P5
#include "cuckoo_wake_sound.h"

'''

for part_name, (start, end) in parts.items():
    part_lines = lines[start:end]
    part_content = ''.join(part_lines)
    
    # Add header, TAG
    content = HEADER + '\n'
    
    # P5 needs extra declarations
    if part_name == 'cuckoo_part5.cc':
        content += P5_FORWARD + '\n'
    
    content += part_content
    
    path = os.path.join(DST, part_name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    
    print(f'{part_name}: lines {start+1}-{end}, {len(part_lines)} lines written')

# Verify: all parts together should have all functions
all_funcs = set()
for part_name in parts:
    path = os.path.join(DST, part_name)
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            if line.strip() and not line.strip().startswith('//') and not line.strip().startswith('*'):
                m = re.search(r'(?:void|int|bool|std::\w+|static)\s+(?:\w+::)*(\w+)\(', line)
                if m:
                    all_funcs.add(m.group(1))

# Check for critical missing methods
critical = ['MotorPowerOn', 'MotorPowerOff', 'SetAngle', 'Sweep', 'Init', 'DecodeSingleFile',
            'PlayUrl', 'PlayOpus', 'PlayPcm', 'Stop', 'Forward', 'Reverse', 'SetSpeed',
            'PlayCuckooSoundSync', 'ReadRaw', 'IsDark', 'DogShowTask', 'LindaShow', 'GardenShow',
            'RunDanceIntro', 'RunDanceLoop', 'RunDanceFinale', 'MusicDanceTick',
            'SetServoAngle', 'SetMotorSpeed', 'PlayOnlineMusic', 'StartShow', 'StopAll',
            'cuckoo_clock_task', 'KidsComeOut', 'KidsRest', 'KidsDanceShow']
for c in critical:
    if c not in all_funcs:
        print(f'WARNING: Method {c}() NOT FOUND in any part!')
print('Split complete!')
