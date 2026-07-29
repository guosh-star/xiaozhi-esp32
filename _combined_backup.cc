// ===== Part 1: 基础模块 (L1-283) - 包含、RTC、Motor、Servo =====
// ============================================
// 布谷鸟钟控制�?(Cuckoo Controller)
//
// 功能概要�?
// - 电机驱动�?路直流电�?+ 1路振动电�?+ 水车/鸟跳
// - 舵机控制：小提琴手臂舵机、小狗尾巴舵�?
// - LED 闪烁�?�?LED 开/关控�?
// - 音频播放：支�?MP3/Opus/PCM 解码，HTTP 下载，Ducking 智能混音
// - 整点/半点报时：开门控�?+ 小鸟跳跃 + 音乐演奏 + 钟声
// - 综合表演：舞蹈电�?+ 小提�?+ 小狗 + 水车 + 鸟跳 + LED
// - 特色 MCP 工具：DogShow、LindaShow、GardenShow
// - 闹钟功能：NVS 持久化，最�?个，支持每天重复
// - 静音模式：支持夜间静音（22:00-6:00�?
// - 在线音乐播放：QQ 音乐代理（Opus/PCM 格式下载�?
// - MCP 工具注册�?1个工具），通过 MCP 协议�?AI 大模型调�?
//
// 双核架构�?
// - Core 0: AI 语音（唤醒词、TTS、Opus 解码�?
// - Core 1: 钟控（cuckoo_clock_task�?50ms tick，时间同步和报时�?
// - 表演/报时通过 xTaskCreatePinnedToCore �?Core 1 执行
// ============================================
#include "cuckoo_controller.h"
#include "cuckoo_bell_sound.h"
#include "../../mcp_server.h"
#include "../../application.h"
#include "../../boards/common/board.h"

#include <esp_log.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_system.h>
#include <esp_http_client.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <cmath>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <math.h>
#include "esp_opus_dec.h"
#include "ogg_demuxer.h"


/**
 * @brief RTC 崩溃日志（上�?软复位不擦除，仅掉电丢失�?
 *
 * RTC_NOINIT_ATTR 标记�?bootloader 跳过该段�?.bss 清零�?
 * 从而在上电复位（非掉电）或 panic 重启后保留现场，用于诊断�?
 *
 * 字段说明�?
 * - magic:       魔数 0xCAFEBABE，标记数据有效（掉电后为随机值）
 * - tick_sec:    重启�?cuckoo_clock_task 累计 tick 秒数
 * - dev_state:   重启�?CuckooStateMachine 的设备状态枚举�?
 * - music_active: 重启�?bg audio 是否在播放（1=�?/ 0=否）
 * - free_heap:   重启�?xPortGetFreeHeapSize() 返回�?
 */
RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t tick_sec;
    uint8_t  dev_state;
    uint8_t  music_active;
    uint32_t free_heap;
} rtc_crash_log;


/**
 * @brief �?QQ 音乐代理 URL 改写�?PCM 格式
 *
 * ESP32 解码 MP3/Opus 消耗较大，服务器端直接输出 16kHz mono PCM
 * 可大幅降�?CPU 负荷。本函数原地改写 URL 中的路径段：
 *   /stream?q=�? �? /pcm?q=�?       （带斜杠前缀�?
 *   /opus?q=�?   �? /pcm?q=�?        （带斜杠前缀�?
 *   stream?q=�?  �? pcm?q=�?         （无前缀，容错）
 *   opus?q=�?    �? pcm?q=�?          （无前缀，容错）
 *
 * 适配 QQ 音乐代理 / 网易云代理等不同 URL 形态�?
 *
 * @param url    原地修改的目�?URL 缓冲�?
 * @param url_sz 缓冲区总大小（字节），防越�?
 */
static void ConvertToPcmUrl(char* url, size_t url_sz) {

    size_t url_len = strlen(url);
    if (url_len >= url_sz) return;

    // /stream?q=... -> /pcm?q=...
    char* pos = strstr(url, "/stream?");
    if (pos) {

        size_t tail = strlen(pos + 7) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 7, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // /opus?q=... -> /pcm?q=...
    pos = strstr(url, "/opus?");
    if (pos) {

        size_t tail = strlen(pos + 5) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 5, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }

    pos = strstr(url, "stream?");
    if (pos && (pos == url || *(pos-1) != '/')) {

        size_t tail = strlen(pos + 6) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 6, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
    pos = strstr(url, "opus?");
    if (pos && (pos == url || *(pos-1) != '/')) {

        size_t tail = strlen(pos + 4) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 4, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
}
#define TAG "CuckooCtrl"

// ============================================
// LEDC PWM 通道分配（共 8 路电�?舵机�?
// ============================================
// Timer 0 (1kHz, 10-bit): TB6612 四路主电�?�?M1 舞蹈 + M3 小狗 + M4 小鸟 + violin_motor
// Timer 1 (50Hz,  14-bit): 舵机 ×2 �?小提琴手�?+ 狗尾
// Timer 2 (1kHz, 10-bit): DRV8833 大门电机 M2
// Timer 3 (10kHz, 8-bit): DRV8833 水车+鸟跳（两个独立单向设备）


// ============================================
// 二路电机初始化（GPIO 直驱模式）：M2(大门) / M3(小狗) / M4(小鸟) �?
// ============================================

/**
 * @brief 电机 GPIO 初始化（通用�?
 *
 * �?in1/in2 配置为推挽输出、无上下拉，不设置中断�?
 * GPIO 直驱模式�?PWM 模式都需要先调用此函数配置引脚电气属性�?
 *
 * @param in1 电机 A 相引脚（正转=高）
 * @param in2 电机 B 相引脚（反转=高）
 */
static void motor_gpio_init(gpio_num_t in1, gpio_num_t in2) {
    gpio_config_t c = { .pin_bit_mask = (1ULL<<in1)|(1ULL<<in2),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}
/**
 * @brief 电机构造函�?- GPIO 直驱模式（非 PWM�?
 *
 * 通过 GPIO 高低电平直接控制电机正反转，无速度调节�?
 * in1 �?+ in2 �?= 正转，in1 �?+ in2 �?= 反转�?
 * 适用�?L9110S 等单极性驱动芯片�?
 *
 * @param in1 正转引脚
 * @param in2 反转引脚
 */
Motor::Motor(gpio_num_t in1, gpio_num_t in2)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(false), max_duty_(0) { motor_gpio_init(in1, in2); Stop(); }
/**
 * @brief 电机构造函�?- PWM 共享定时器模式（10-bit / 1kHz�?
 *
 * 使用 LEDC_TIMER_MOTOR（Timer 0）定时器�?0-bit 分辨率（max_duty=1023），
 * 基频 1kHz。两�?LEDC 通道分别控制正转/反转占空比：
 *   - speed > 0 �?ch1=duty, ch2=0（正转）
 *   - speed < 0 �?ch1=0, ch2=duty（反转）
 *
 * static t0_done 守卫确保 Timer 0 全局仅初始化一次，后续 Motor 实例复用�?
 * 适用�?TB6612 四路主电机（M1 舞蹈 / M3 小狗 / M4 小鸟 / violin_motor）�?
 *
 * @param in1 正向 GPIO 引脚
 * @param in2 反向 GPIO 引脚
 * @param ch1 正向 LEDC 通道�?
 * @param ch2 反向 LEDC 通道�?
 * @param sm  LEDC 速度模式（通常�?LEDC_LOW_SPEED_MODE�?
 */
Motor::Motor(gpio_num_t in1, gpio_num_t in2, ledc_channel_t ch1, ledc_channel_t ch2, ledc_mode_t sm)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(true), ledc_channel_(ch1), ledc_channel2_(ch2),
      ledc_timer_(LEDC_TIMER_MOTOR), speed_mode_(sm), max_duty_(1023) {
    static bool t0_done = false;
    if (!t0_done) { ledc_timer_config_t tc = { .speed_mode=sm, .duty_resolution=LEDC_TIMER_10_BIT,
        .timer_num=LEDC_TIMER_MOTOR, .freq_hz=1000, .clk_cfg=LEDC_AUTO_CLK }; ledc_timer_config(&tc); t0_done=true; }
    ledc_channel_config_t c1 = { .gpio_num=in1, .speed_mode=sm, .channel=ch1,
        .intr_type=LEDC_INTR_DISABLE, .timer_sel=LEDC_TIMER_MOTOR, .duty=0 };
    ledc_channel_config(&c1);
    ledc_channel_config_t c2 = { .gpio_num=in2, .speed_mode=sm, .channel=ch2,
        .intr_type=LEDC_INTR_DISABLE, .timer_sel=LEDC_TIMER_MOTOR, .duty=0 };
    ledc_channel_config(&c2);
    Stop();
}

/**
 * @brief 电机构造函�?- PWM 独立定时器模式（8-bit / 10kHz�?
 *
 * 使用自定�?LEDC 定时器编号，8-bit 分辨率（max_duty=255），基频 10kHz�?
 * static custom_done[timer] 守卫确保每个定时器编号仅初始化一次，
 * 不同编号的电机可独立调速互不干扰�?
 *
 * 适用范围�?
 * - DRV8833 �?水车（正�? 独立单向=一脚接地）+ 鸟跳（负�? 电磁铁一脚接地），Timer 3
 * - DRV8833 �?大门电机 M2，Timer 2
 * - 其他需要独立频率或�?1kHz 基频的电�?
 *
 * @param in1   正向 GPIO 引脚
 * @param in2   反向 GPIO 引脚
 * @param ch1   正向 LEDC 通道�?
 * @param ch2   反向 LEDC 通道�?
 * @param timer LEDC 定时器编号（独立�?LEDC_TIMER_MOTOR�?
 * @param sm    LEDC 速度模式
 */
Motor::Motor(gpio_num_t in1, gpio_num_t in2, ledc_channel_t ch1, ledc_channel_t ch2, ledc_timer_t timer, ledc_mode_t sm)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(true), ledc_channel_(ch1), ledc_channel2_(ch2),
      ledc_timer_(timer), speed_mode_(sm), max_duty_(255) {
    static bool custom_done[4] = {false};
    if (!custom_done[timer]) { ledc_timer_config_t tc = { .speed_mode=sm, .duty_resolution=LEDC_TIMER_8_BIT,
        .timer_num=timer, .freq_hz=10000, .clk_cfg=LEDC_AUTO_CLK }; ledc_timer_config(&tc); custom_done[timer]=true; }
    ledc_channel_config_t c1 = { .gpio_num=in1, .speed_mode=sm, .channel=ch1,
        .intr_type=LEDC_INTR_DISABLE, .timer_sel=timer, .duty=0 };
    ledc_channel_config(&c1);
    ledc_channel_config_t c2 = { .gpio_num=in2, .speed_mode=sm, .channel=ch2,
        .intr_type=LEDC_INTR_DISABLE, .timer_sel=timer, .duty=0 };
    ledc_channel_config(&c2);
    Stop();
}
/**
 * @brief 设置电机速度
 *
 * @param speed -100（全速反转）�?100（全速正转）�?=停止
 * GPIO 直驱�?PWM 占空比，自动判断 use_pwm_�?
 */
void Motor::SetSpeed(int speed) {
    if (speed == 0) { Stop(); return; }
    int spd = abs(speed); if (spd > 100) spd = 100;
    if (use_pwm_) {
        uint32_t duty = (spd * max_duty_) / 100;
        if (speed < 0) { ledc_set_duty(speed_mode_, ledc_channel_, 0); ledc_set_duty(speed_mode_, ledc_channel2_, duty); }
        else { ledc_set_duty(speed_mode_, ledc_channel_, duty); ledc_set_duty(speed_mode_, ledc_channel2_, 0); }
        ledc_update_duty(speed_mode_, ledc_channel_);
        ledc_update_duty(speed_mode_, ledc_channel2_);
    } else {
        if (speed > 0) { gpio_set_level(in1_pin_, 1); gpio_set_level(in2_pin_, 0); }
        else { gpio_set_level(in1_pin_, 0); gpio_set_level(in2_pin_, 1); }
    }
}
/**
 * @brief 停止电机（所有通道输出置零�?
 *
 * GPIO 直驱：两脚同时置低（制动）�?
 * PWM 模式：两通道 duty 置零�?update�?
 * �?SetSpeed(0) 等效，调用此函数�?
 */
void Motor::Stop() {
    if (use_pwm_) { ledc_set_duty(speed_mode_, ledc_channel_, 0); ledc_set_duty(speed_mode_, ledc_channel2_, 0);
        ledc_update_duty(speed_mode_, ledc_channel_); ledc_update_duty(speed_mode_, ledc_channel2_); }
    else { gpio_set_level(in1_pin_, 0); gpio_set_level(in2_pin_, 0); }
}
/**
 * @brief 电机正转
 *
 * 调用 SetSpeed: speed>0 �?原值，�? �?默认 100%�?
 * 阻止负值变成反转，确保调用方意图明确�?
 *
 * @param speed 速度百分比（1~100），默认 100
 */
void Motor::Forward(int speed) { SetSpeed(speed > 0 ? speed : 100); }
/**
 * @brief 电机反转
 *
 * 调用 SetSpeed: speed>0 �?-speed，≤0 �?-100�?
 * 阻止正值变成正转，确保调用方意图明确�?
 *
 * @param speed 速度百分比（1~100），默认 100
 */
void Motor::Reverse(int speed) { SetSpeed(speed > 0 ? -speed : -100); }

// ============================================

/**
 * @brief 舵机构造函�?�?SG90 兼容 50Hz PWM�?4-bit 分辨率）
 *
 * 使用 LEDC_TIMER_SERVO（Timer 1），基频 50Hz�?4-bit 分辨率（max_duty=16383）�?
 * static servo_timer_inited 守卫确保 Timer 1 全局仅初始化一次，
 * 后续 Servo 实例仅配置通道不复初始化定时器�?
 *
 * PWM 脉冲宽度范围�?.5ms�?°）~ 2.5ms�?80°），对应 duty 409~2048�?
 * 构造完成后自动归位�?90°（中位）�?
 *
 * @param pin     舵机信号 GPIO
 * @param channel LEDC 通道号（小提琴用 CH0，狗尾用 CH1�?
 * @param speed_mode LEDC 速度模式（通常 LEDC_LOW_SPEED_MODE�?
 */
Servo::Servo(gpio_num_t pin, ledc_channel_t channel, ledc_mode_t speed_mode)
    : pin_(pin), angle_(90), ledc_channel_(channel), speed_mode_(speed_mode) {


    static bool servo_timer_inited = false;
    if (!servo_timer_inited) {
        ledc_timer_config_t timer_conf = {
            .speed_mode = speed_mode_,
            .duty_resolution = LEDC_TIMER_14_BIT,
            .timer_num = LEDC_TIMER_SERVO,
            .freq_hz = 50,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer_conf);
        servo_timer_inited = true;
    }

    ledc_channel_config_t ch_conf = {
        .gpio_num = pin_,
        .speed_mode = speed_mode_,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_SERVO,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    ledc_channel_config(&ch_conf);

    SetAngle(90);
}

/**
 * @brief 设置舵机角度 (0°~180°)
 *
 * @param angle 目标角度，自动钳位到 [0, 180]
 * 将角度转换为 50Hz PWM 占空�?(409~2048 对应 0°~180°)
 */
void Servo::SetAngle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    angle_ = angle;
    uint32_t duty = (uint32_t)(409 + (float)(angle) / 180.0f * (2048 - 409));
    ledc_set_duty(speed_mode_, ledc_channel_, duty);
    ledc_update_duty(speed_mode_, ledc_channel_);
}

/**
 * @brief 舵机平滑扫描
 *
 * @param from 起始角度
 * @param to 目标角度
 * @param duration_ms 持续时间（毫秒），每 20ms 一�?
 */
void Servo::Sweep(int from, int to, int duration_ms) {
    if (from == to) { SetAngle(to); return; }
    int steps = duration_ms / 20;
    if (steps < 1) steps = 1;
    float delta = (float)(to - from) / steps;
    for (int i = 0; i <= steps; i++) {
        SetAngle(from + (int)(delta * i));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================
// 水车 + 鸟跳（两个独立单向设备，DRV8833 双通道，Timer 3 / 10kHz�?
//
// water_bird_ 是单�?Motor 实例，通过正负速度区分功能�?
//   SetSpeed(WATER_WHEEL_SPEED)  �?ch0 输出 �?水车旋转
//   SetSpeed(-100)                �?ch1 输出 �?鸟跳一次（短脉冲）
//
// 使用 Motor 构造函�?#3（自定义定时器，8-bit），
// 与水车主电机群（TB6612 Timer 0 1kHz）隔离，互不干扰�?
// 配置�?config.h: MOTOR_WATER_BIRD_IN1/IN2, LEDC_CH_WATER/JUMP, LEDC_TIMER_WATER
// ============================================

// ============================================
// MP3 播放器（仅析构函数在此文件，其余实现�?part2�?
//
// 能力概览�?
// - DecodeSingleFile: 本地 MP3/AAC 解码阻塞播放
// - PlayUrl: HTTP 流式 MP3 下载 + 解码播放
// - PlayPcm:  原始 PCM 数据�?6kHz/mono/s16）直接写入音频输�?
// - PlayOpus: OGG/Opus 容器流解码播放（低码率语音场景）
// - Ducking: AI 说话时音乐音量从 100% 平滑降至 20%（~400ms�?
// - 混音:   LoadDogBark 将预编译 PCM 叠加到播放流�?
// ============================================

/**
 * @brief 析构函数，停止播放并等待异步下载任务退出（5秒超时）
 */
Mp3Player::~Mp3Player() {
    stop_requested_ = true;
    // 等待最�?秒让下载线程退出（recv 返回 error 后关 socket�?
    for (int i = 0; i < 100 && is_playing_; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  // 总共5秒超�?
    }
    if (play_task_) {
        vTaskDelete(play_task_);
        play_task_ = nullptr;
    }
    is_playing_ = false;
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }
}
// ===== Part 2: Mp3Player �?(L284-1924 / 原始行号) =====
// ============================================
// Mp3Player �?音频播放引擎
//
// 负责所有音频的下载、解码、后处理与输出：
// - 本地 MP3 解码（DecodeSingleFile）：asset 分区 �?MP3 decoder �?PCM �?输出
// - HTTP �?MP3（PlayUrl）：批量下载 �?重采样至 24kHz/mono �?Ducking �?输出
// - HTTP PCM 直通（PlayPcm）：无解码开销 �?Ducking �?PushRawPcmToPlayback
// - BSD Socket OGG/Opus（PlayOpus）：低码率音乐流，双路径（socket / serial fallback�?
// - 闹钟铃声（PlayAlarmRing）：纯软�?PCM 合成（正弦波 beep�?
// - Ding-dong 音效（PlayDingDong）：预留接口
//
// 关键设计�?
// - Ducking: AI 说话时音乐音量平滑淡出至 20%�?00ms），说完恢复
// - 狗叫混音: LoadDogBark �?PCM 叠加到当前解码帧上（不打断播放）
// - 任务模型: 每次播放 spawn 独立 FreeRTOS task（stack 4-24KB 视路径而定�?
// - 内存策略: 优先 SPIRAM �?回退内部 heap（多级降级）
// ============================================
/**
 * @brief 初始�?Mp3Player，绑�?assets 分区
 *
 * 设置 assets_ 指针后，后续 DecodeSingleFile / DecodeToBuffer �?
 * 本地播放方法即可通过 assets_->GetAssetData() 读取 MP3 文件�?
 *
 * @param assets 指向应用�?Assets 实例的指�?
 */
void Mp3Player::Init(Assets* assets) {
    assets_ = assets;
    ESP_LOGI(TAG, "Mp3Player initialized (async task-based software decode)");
}

/**
 * @brief Ducking 状态更新（简化版，PlayUrl / PlayPcm 共用�?
 *
 * 仅处理淡出方向：AI 说话 �?400ms �?100%�?0%�?
 * 恢复逻辑由调用方�?duck_state 状态机负责�?
 *
 * 状态追踪：
 * - ducking_gain_ >= 1.0   �?空闲，AI 开始说话时触发淡出
 * - 0.2 < ducking_gain_ < 1.0 �?淡出�?
 * - ducking_gain_ == 0.2    �?淡出完成，保�?
 */
void Mp3Player::UpdateDuckingState() {
    auto& app = Application::GetInstance();
    auto dev_state = app.GetDeviceState();
    if (dev_state == kDeviceStateSpeaking && ducking_gain_ >= 1.0f) {
        ducking_gain_ = 1.0f;
        ducking_start_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Ducking: AI speaking, fading out");
    }
    if (ducking_gain_ < 1.0f) {
        if (dev_state != kDeviceStateSpeaking) {
            ducking_gain_ = 1.0f; ducking_start_us_ = 0;
        } else {
            int64_t elapsed = esp_timer_get_time() - ducking_start_us_;
            ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
            if (ducking_gain_ < 0.2f) {
                ducking_gain_ = 0.2f;
                ESP_LOGI(TAG, "Ducking: fade complete at 20%%");
            }
        }
    }
}

/**
 * @brief 加载狗叫 PCM 数据到混音缓冲区
 *
 * 将预编译�?PCM 采样存入 bark_pcm_，在音频输出循环中自动叠加到正在播放�?
 * 音乐数据上（不打断音乐播放）。若已有狗叫在混音中，则等待完成后加载新数据�?
 *
 * @param pcm 狗叫 PCM 数据指针
 * @param num_samples 采样点数
 */
void Mp3Player::LoadDogBark(const int16_t* pcm, size_t num_samples) {
    while (bark_active_) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bark_pcm_ = std::vector<int16_t>(pcm, pcm + num_samples);
    bark_total_ = num_samples;
    bark_offset_ = 0;
    bark_active_ = true;
    ESP_LOGI(TAG, "DogBark: loaded %u samples, mixing into music output", (unsigned)num_samples);
}

/**
 * @brief �?PCM 缓冲逐采样乘 Ducking 增益（带限幅�?
 *
 * 增益值来�?ducking_gain_（UpdateDuckingState 更新），
 * 结果限幅�?±30000（留 8% 余量防溢出）�?
 * 若增�?�?.0 则直接返回，不修改缓冲�?
 *
 * @param pcm         待处理的 PCM 缓冲（原地修改）
 * @param num_samples 采样点数
 */
void Mp3Player::ApplyDuckingGain(int16_t* pcm, size_t num_samples) {
    float g = ducking_gain_;
    if (g >= 1.0f) return;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t s = (int32_t)(pcm[i] * g);
        if (s > 30000) s = 30000;
        if (s < -30000) s = -30000;
        pcm[i] = (int16_t)s;
    }
}

/**
 * @brief 本地 MP3 解码播放（同步阻塞，�?assets 分区读取�?
 *
 * 完整流程：读取文�?�?跳过 ID3v2 �?初始�?MP3 decoder �?逐帧解码
 *          �?Ducking 状态机�?态：空闲/淡出/压低/恢复）→ 狗叫混音叠加
 *          �?OutputRawPcm 推送到音频输出流水�?
 *
 * Ducking 仅在 disable_ducking_ == false 时生效�?
 *
 * @param index MP3 文件编号�?001~0012=歌曲, 0013=报时铃声, 0015=琳达, 0016=花园�?
 * @return 1=完整播放, 0=�?stop_requested_ 中断或播放失�?
 */
int Mp3Player::DecodeSingleFile(int index) {
    if (!assets_) {
        ESP_LOGE(TAG, "Assets not initialized");
        return 0;
    }

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

    // �?assets 分区读取 MP3 文件数据
    void* mp3_data = nullptr;
    size_t mp3_size = 0;
    if (!assets_->GetAssetData(filename, mp3_data, mp3_size)) {
        ESP_LOGE(TAG, "Failed to get asset: %s", filename);
        return 0;
    }

    if (!mp3_data || mp3_size == 0) {
        ESP_LOGE(TAG, "Empty asset: %s", filename);
        return 0;
    }

    ESP_LOGI(TAG, "Playing %s (%lu bytes)", filename, (unsigned long)mp3_size);

    // 跳过 ID3v2 标签
    uint8_t* mp3_start = (uint8_t*)mp3_data;
    size_t mp3_data_size = mp3_size;
    if (mp3_size > 10 && memcmp(mp3_start, "ID3", 3) == 0) {
        uint32_t id3_size = ((mp3_start[6] & 0x7F) << 21) | ((mp3_start[7] & 0x7F) << 14)
                          | ((mp3_start[8] & 0x7F) << 7)  |  (mp3_start[9] & 0x7F);
        size_t skip = 10 + id3_size;
if (skip < mp3_size - 1024) {  // 确保跳过ID3后有足够空间
            mp3_start += skip;
            mp3_data_size -= skip;
            ESP_LOGI(TAG, "Skipped ID3v2 tag: %lu bytes", (unsigned long)skip);
        }
    }


    // ---- 解码器初始化：每次播放前重建 MP3 解码器实例，防止残留状�?----
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return 0;
    }

    // ---- 解码状态变�?----
    esp_audio_dec_info_t dec_info = {};
    bool info_ready = false;    // 首帧解码成功后获取采样率/声道
    int sample_rate = 22050;    // 默认采样率（首帧解码后从 dec_info 读取实际值）
    int channels = 1;           // 默认单声�?

    // ---- 逐块喂入解码�?----
    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 50;  // 连续解码失败 50 次则放弃

    while (remaining > 0 && !stop_requested_) {

        // 每次最多喂 kInputBufSize 字节，解码器内部会累积未消费数据
        size_t in_len = (remaining < kInputBufSize) ? remaining : kInputBufSize;
        memcpy(input_buf_, input_ptr, in_len);

        esp_audio_dec_in_raw_t raw;
        raw.buffer = input_buf_;
        raw.len = in_len;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;


        esp_audio_dec_out_frame_t frame;
        frame.buffer = output_buf_;
        frame.len = kOutputBufSize;
        frame.needed_size = 0;
        frame.decoded_size = 0;


        ret = esp_mp3_dec_decode(mp3_dec_handle_, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK) {

            if (!info_ready && frame.decoded_size > 0) {
                sample_rate = dec_info.sample_rate ? dec_info.sample_rate : 22050;
                channels = dec_info.channel ? dec_info.channel : 1;
                ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %.1f kbps",
                         sample_rate, channels, dec_info.bitrate / 1000.0f);
                info_ready = true;
            }


            if (frame.decoded_size > 0 && !stop_requested_) {
                int16_t* pcm_data = (int16_t*)frame.buffer;
                size_t num_samples = frame.decoded_size / sizeof(int16_t);

                auto& app = Application::GetInstance();

 // ---- Ducking 4态机（仅 DecodeSingleFile 使用�?---
                // 0=空闲 �?AI说话 �?1=淡出(400ms,100%�?0%) �?2=压低
                // �?AI停止 �?3=恢复(400ms,20%�?00%) �?0。任意状态遇AI说话→跳�?
                static int duck_state = 0;
                static int64_t duck_transition_us = 0;
                bool ai_speaking = (app.GetDeviceState() == kDeviceStateSpeaking);

                if (disable_ducking_) {
                    // 禁用 Ducking: 始终满音�?
                    ducking_gain_ = 1.0f;
                } else if (ai_speaking && duck_state == 0) {
                    // 空闲状态检测到 AI 说话 �?开始淡�?
                    duck_state = 1;
                    duck_transition_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "DecodeSingleFile: AI speaking, fading out");
                }

                if (duck_state == 1) {
                    // 淡出�? 线性插值从 1.0 降至 0.2�?00ms 时长�?
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ <= 0.2f) {
                        ducking_gain_ = 0.2f;
                        duck_state = 2;  // 淡出完成 �?已压�?
                    }
                    if (!ai_speaking) {
                        // AI 提前停止（误判）�?立即恢复
                        duck_state = 3;
                        duck_transition_us = esp_timer_get_time();
                    }
                } else if (duck_state == 2) {
                    ducking_gain_ = 0.2f;  // 保持 20% 音量
                    if (!ai_speaking) {
                        duck_state = 3;  // AI 说完 �?开始恢�?
                        duck_transition_us = esp_timer_get_time();
                    }
                } else if (duck_state == 3) {
                    // 恢复�? 线性插值从 0.2 升至 1.0�?00ms 时长�?
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 0.2f + 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ >= 1.0f) {
                        ducking_gain_ = 1.0f;
                        duck_state = 0;  // 恢复完成 �?空闲
                        ESP_LOGI(TAG, "DecodeSingleFile: false trigger, restored");
                    }
                    if (ai_speaking) {
                        // 恢复期间 AI 又说�?�?重新淡出
                        duck_state = 1;
                        duck_transition_us = esp_timer_get_time();
                    }
                }

                // 应用 Ducking 增益 + 数字限幅（防止溢出到 ±32768 以外�?int16 范围�?
                float gain = ducking_gain_;
                for (size_t i = 0; i < num_samples; i++) {
                    int32_t s = (int32_t)(pcm_data[i] * gain);
                    if (s > 30000) s = 30000;   // 硬限幅至 ±30000（留 8% 余量�?
                    if (s < -30000) s = -30000;
                    pcm_data[i] = (int16_t)s;
                }


                // ---- 狗叫混音叠加：将 bark_pcm_ 叠加到当前帧 PCM 上（不打断音乐）----
                if (bark_active_) {
                    size_t to_mix = num_samples;
                    if (bark_offset_ + to_mix > bark_total_) {
                        to_mix = bark_total_ - bark_offset_;  // 剩余不足一帧，混到尾巴
                    }
                    for (size_t i = 0; i < to_mix; i++) {
                        int32_t mixed = (int32_t)pcm_data[i] + (int32_t)bark_pcm_[bark_offset_ + i];
                        if (mixed > 32767) mixed = 32767;      // 叠加后限�?int16
                        else if (mixed < -32768) mixed = -32768;
                        pcm_data[i] = (int16_t)mixed;
                    }
                    bark_offset_ += to_mix;
                    if (bark_offset_ >= bark_total_) {
                        bark_active_ = false;  // 全部混完
                        ESP_LOGI(TAG, "DogBark: mix done (%u samples)", (unsigned)bark_total_);
                    }
                }

                // 推送最�?PCM 到音频输出流水线
                app.GetAudioService().OutputRawPcm(pcm_data, num_samples, sample_rate);

                // 计算本帧的播放时长（用于 speed 控制�?
                int play_ms = (int)(num_samples * 1000 / sample_rate / (channels > 0 ? channels : 1));
                if (play_ms < 10) play_ms = 10;
                vTaskDelay(pdMS_TO_TICKS(play_ms));
            }

            // 解码器消费的字节数（raw.consumed），据此推进输入指针
            size_t consumed = raw.consumed;
            if (consumed == 0) {
                consumed = 1;  // 极短文件逐字节前进，防止死循�?
                if (consumed > remaining) consumed = remaining;
            }
            consecutive_errors = 0;  // 解码成功，重置错误计�?
            input_ptr += consumed;
            remaining -= consumed;

        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer too small, needed %lu", (unsigned long)frame.needed_size);
            break;  // 输出缓冲区不足，放弃后续解码
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            ESP_LOGE(TAG, "Unsupported MP3 format (stopping)");
            break;  // 不支持的格式，停�?
        } else {
            // 解码失败: 递增错误计数器，跳过当前帧继续尝�?
            consecutive_errors++;
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "Too many consecutive decode errors (%d), aborting", consecutive_errors);
                break;  // 连续失败 50 �?�?放弃播放
            }
            ESP_LOGW(TAG, "Decode error %d, skipping frame", ret);
            size_t skip = raw.consumed ? raw.consumed : 1;
            if (skip > remaining) skip = remaining;
            input_ptr += skip;
            remaining -= skip;
        }
    }


    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }


    auto& app = Application::GetInstance();
    if (stop_requested_) {
        app.GetAudioService().FlushOutputDma();
    }
    app.GetAudioService().SetOutputMuted(false);

    ESP_LOGI(TAG, "Finished playing %s (stopped=%d)", filename, stop_requested_.load() ? 1 : 0);
    return !stop_requested_;
}

// ============================================

/**
 * @brief 异步 HTTP 流式 MP3 播放
 *
 * spawn PlayUrlTask 在后台任务中完成 HTTP GET �?批量下载 �?MP3 解码�?
 * 解码�?PCM 经立体声→单声道混音 + 升采样至 24kHz + Ducking 增益后输出�?
 *
 * @param url HTTP MP3 资源地址
 * @return 0=成功启动, -1=URL 为空
 */
int Mp3Player::PlayUrl(const char* url) {
    if (!url || url[0] == '\0') return -1;

    ESP_LOGI(TAG, "PlayUrl: launching background task for %s", url);


    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PlayUrlCtx();
    ctx->self = this;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->url[sizeof(ctx->url) - 1] = '\0';

    xTaskCreate(PlayUrlTask, "mp3_url", 8192, ctx, 5, NULL);
        return 0;  // 启动异步任务，返回成�?
}

/**
 * @brief 验证 4 字节是否为合�?MPEG Audio Layer 3 帧头
 *
 * 检查规则：
 * - 同步�? 0xFFE
 * - Layer: 必须�?Layer 3 (01)
 * - Bitrate: �?0x00 �?0x0F（避�?free/reserved�?
 * - Sample rate: �?reserved (11)
 *
 * @param p 指向疑似帧头起始�?4 字节
 * @return true=合法帧头, false=无效
 */
static bool IsValidMpegHeader(const uint8_t* p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int layer = (p[1] >> 1) & 0x03;
    if (layer != 1) return false;  // Layer 3 only
    int bitrate = (p[2] >> 4) & 0x0F;
    if (bitrate == 0 || bitrate == 0x0F) return false;
    int srate = (p[2] >> 2) & 0x03;
    if (srate == 0x03) return false;
    return true;
}

/**
 * @brief PlayUrlTask �?HTTP 流式 MP3 后台下载+解码任务
 *
 * 流程：HTTP GET �?分批下载�?56KB~4MB）→ 跳过 ID3v2 �?检�?MP3 帧头确定采样�?
 *      �?主循环：逐帧解码 �?立体声→单声�?�?线性升采样�?24kHz �?Ducking �?输出
 *
 * 内存策略：优�?SPIRAM 分配 batch 缓冲区，失败后降级至 64KB 系统堆�?
 * 重采样缓冲区：优�?SPIRAM �?失败�?malloc 系统堆（有警告）�?
 *
 * @param arg PlayUrlCtx{self, url} 结构体指针（任务�?delete�?
 */
void Mp3Player::PlayUrlTask(void* arg) {
    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = (PlayUrlCtx*)arg;
    Mp3Player* self = ctx->self;
    char url[512];
    strncpy(url, ctx->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    delete ctx;

    ESP_LOGI(TAG, "PlayUrl: batch-stream task for %s", url);

    auto& app = Application::GetInstance();


        // === 策略：HTTP GET 下载 MP3 �?分批解码 �?重采样至24000Hz单声�?�?Ducking �?输出 ===
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "PlayUrl: client init failed"); vTaskDelete(NULL); return; }

    esp_err_t e = esp_http_client_open(client, 0);
    if (e != ESP_OK) { ESP_LOGE(TAG, "PlayUrl: open failed %d", e); esp_http_client_cleanup(client); vTaskDelete(NULL); return; }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "PlayUrl: HTTP %d, Len=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "PlayUrl: HTTP err %d", status);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }


    void* dec_handle = nullptr;
    esp_audio_err_t dec_ret = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PlayUrl: decoder fail %d", dec_ret);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }

    self->is_playing_ = true;
    app.GetAudioService().SetOutputMuted(true);


    size_t batch_size = 256 * 1024;
    if (content_length > 0 && content_length < 8 * 1024 * 1024) {
batch_size = content_length; // 内容长度可靠，全部一次下�?
if (batch_size > 4 * 1024 * 1024) batch_size = 4 * 1024 * 1024; // 限制4MB
    // 内存分配降级策略：SPIRAM 4MB �?SPIRAM 256KB �?系统�?64KB �?失败退�?
        ESP_LOGI(TAG, "PlayUrl: batch=%dKB (content=%dKB)", (int)(batch_size/1024), content_length/1024);
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    if (!buf) {

        batch_size = 256 * 1024;
        buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    }
    if (!buf) {
        batch_size = 64 * 1024;
        buf = (uint8_t*)malloc(batch_size);  // last resort fallback
    }
    if (!buf) {
        ESP_LOGE(TAG, "PlayUrl: alloc failed");
        esp_mp3_dec_close(dec_handle); esp_http_client_close(client); esp_http_client_cleanup(client);
        self->is_playing_ = false; app.GetAudioService().SetOutputMuted(false);
        vTaskDelete(NULL); return;
    }


    const int kOutRate = 24000;
                // MP3 最差情况缓冲区: 1152立体声从8000�?4000Hz升采�?


    const int kResampBufSamples = 4096;
    int16_t* resample_buf1 = (int16_t*)heap_caps_malloc(kResampBufSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t* resample_buf2 = (int16_t*)heap_caps_malloc(kResampBufSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!resample_buf1) resample_buf1 = (int16_t*)malloc(kResampBufSamples * sizeof(int16_t));
    if (!resample_buf2) resample_buf2 = (int16_t*)malloc(kResampBufSamples * sizeof(int16_t));
    ESP_LOGI(TAG, "PlayUrl: resamp bufs %s", (resample_buf1 && resample_buf2) ? "ok" : "WARN");

    esp_audio_dec_info_t dec_info = {};
    size_t total_dl = 0;
    int sample_rate = 0, play_ms = 0, batch_seq = 0;
    size_t mp3_start = 0, rem = 0;
    uint8_t* ptr = nullptr;

    // ============================================
    // 跳过 ID3v2 标签
    // ============================================
    size_t batch_len = 0;
    int err_cnt = 0;
    while (batch_len < batch_size && !self->stop_requested_) {
        int r = esp_http_client_read(client, (char*)(buf + batch_len), batch_size - batch_len);
        if (r > 0) { batch_len += r; err_cnt = 0; }
        else if (r == 0) break;
        else { if (++err_cnt > 3) break; vTaskDelay(pdMS_TO_TICKS(50)); }
    }
    total_dl = batch_len;
    if (batch_len == 0) goto cleanup;
    ESP_LOGI(TAG, "PlayUrl: batch#1 dl=%dKB", (int)(batch_len/1024));

    // 跳过 ID3v2 标签
        // MP3 文件可能带有 ID3v2 元数据标签（10字节�?+ 内容），需要跳过才能正确解�?
    mp3_start = 0;
    if (batch_len > 10 && memcmp(buf, "ID3", 3) == 0) {
        uint32_t id3_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14)
                          | ((buf[8] & 0x7F) << 7)  |  (buf[9] & 0x7F);
        mp3_start = 10 + id3_size;
if (mp3_start > batch_len - 1024) mp3_start = 0; // ID3标签太大，从头开�?
        ESP_LOGI(TAG, "PlayUrl: ID3v2 %luB, MP3 @ %u", id3_size, (unsigned)mp3_start);
    }


    for (size_t k = mp3_start; k + 3 < batch_len; k++) {
        if (buf[k] == 0xFF && (buf[k+1] & 0xE0) == 0xE0) {
            int ver = (buf[k+1] >> 3) & 0x03;
            int lay = (buf[k+1] >> 1) & 0x03;
            if (lay != 1) continue; // ֻҪ Layer 3
            int sri = (buf[k+2] >> 2) & 0x03;
            if      (ver == 3) { static const int r[]={44100,48000,32000,0}; sample_rate = r[sri]; }
            else if (ver == 2) { static const int r[]={22050,24000,16000,0}; sample_rate = r[sri]; }
            else if (ver == 0) { static const int r[]={11025,12000,8000,0};  sample_rate = r[sri]; }
            if (sample_rate > 0) { ESP_LOGI(TAG, "PlayUrl: MP3 %dHz @ %u", sample_rate, (unsigned)k); break; }
        }
    }
    if (sample_rate == 0) { sample_rate = 44100; ESP_LOGW(TAG, "PlayUrl: no sync header, default %dHz", sample_rate); }


    ptr = buf + mp3_start;
    rem = batch_len - mp3_start;
    // ====================================================================
    // 主解码循环：持续下载 �?分批解码 �?重采�?�?Ducking �?推送到音频输出
    // ====================================================================
    batch_seq = 1;

    while (1) {
        // ---- 内层：从已下载的 batch 缓冲区逐块喂入 MP3 解码�?----
                // 内层：从 batch 缓冲区逐块喂入 MP3 解码器，每次最�?kInputBufSize 字节
        while (rem > 0 && !self->stop_requested_) {
            size_t feed = (rem < self->kInputBufSize) ? rem : self->kInputBufSize;
            memcpy(self->input_buf_, ptr, feed);

            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = self->input_buf_;
            raw.len = feed;
            raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = self->output_buf_;
            frame.len = self->kOutputBufSize;

            dec_ret = esp_mp3_dec_decode(dec_handle, &raw, &frame, &dec_info);

            if (dec_ret == ESP_AUDIO_ERR_OK) {
                if (frame.decoded_size > 0 && !self->stop_requested_) {
                    // ---- Ducking: AI 说话时平滑淡出音乐（400ms 内从 100% 降至 20%�?---
                    auto dev_state = app.GetDeviceState();
                    if (dev_state == kDeviceStateSpeaking && self->ducking_gain_ >= 1.0f) {
                        self->ducking_gain_ = 1.0f;
                        self->ducking_start_us_ = esp_timer_get_time();
                        ESP_LOGI(TAG, "PlayUrl: AI speaking, fading out");
                    }
                    if (self->ducking_gain_ < 1.0f) {
                        if (dev_state != kDeviceStateSpeaking) {
                            self->ducking_gain_ = 1.0f; self->ducking_start_us_ = 0;
                        } else {
                            int64_t elapsed = esp_timer_get_time() - self->ducking_start_us_;
                            self->ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                            if (self->ducking_gain_ < 0.2f) {
                                self->ducking_gain_ = 0.2f;
                            }
                        }
                    }

                    // ---- 解码后处理：立体声→单声�?+ 升采样至 24000Hz + Ducking 增益 ----
                    int16_t* pcm = (int16_t*)frame.buffer;
                    size_t ns = frame.decoded_size / sizeof(int16_t);  // stereo sample count
                    size_t mono_ns = ns / 2;



                    if (sample_rate != kOutRate && mono_ns > 0) {
                        // 步骤1: 立体声→单声道（左右声道取平均）
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }

                        // 步骤2: 线性插值升采样至目标采样率
                        float ratio = (float)kOutRate / (float)sample_rate;
                        size_t out_ns = (size_t)(mono_ns * ratio);
                        for (size_t i = 0; i < out_ns; i++) {
                            float sp = i / ratio;
                            size_t idx = (size_t)sp;
                            float frac = sp - idx;
                            if (idx + 1 < mono_ns)
                                resample_buf2[i] = (int16_t)(resample_buf1[idx] * (1.0f - frac) + resample_buf1[idx+1] * frac);
                            else
                                resample_buf2[i] = resample_buf1[idx];
                        }

                        // 步骤3: 应用 Ducking 增益 + 限幅
                        float g = self->ducking_gain_;
                        for (size_t i = 0; i < out_ns; i++) {
                            int32_t s = (int32_t)(resample_buf2[i] * g);
                            if (s > 30000) s = 30000;
                            if (s < -30000) s = -30000;
                            resample_buf2[i] = (int16_t)s;
                        }
                        // 步骤4: 推�?PCM 到音频输出流水线
                        app.GetAudioService().OutputRawPcm(resample_buf2, out_ns, kOutRate);
                    } else {
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }

                        float g = self->ducking_gain_;
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t s = (int32_t)(resample_buf1[i] * g);
                            if (s > 30000) s = 30000;
                            if (s < -30000) s = -30000;
                            resample_buf1[i] = (int16_t)s;
                        }
                        app.GetAudioService().OutputRawPcm(resample_buf1, mono_ns, sample_rate);
                    }
                    play_ms += (int)(ns * 1000 / sample_rate);
                }

                size_t c = dec_info.frame_size;
                if (c == 0) c = raw.consumed;  // 退回标�?
                if (c == 0) c = 1;
                if (c >= rem) { rem = 0; } else { ptr += c; rem -= c; }
            } else if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (raw.consumed > 0 && raw.consumed < rem) { ptr += raw.consumed; rem -= raw.consumed; }
                else if (raw.consumed == 0) {

                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 < rem) { ptr += scan; rem -= scan; }
else { rem = 0; break; }  // 未找到同步帧头，停止
                }
                if (rem < 1024) break;
            } else {
                if (raw.consumed > 0) {
                    if (raw.consumed >= rem) { rem = 0; }
                    else { ptr += raw.consumed; rem -= raw.consumed; }
                } else {

                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 >= rem) { rem = 0; break; }
                    ptr += scan; rem -= scan;
                }
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d rem=%u stop=%d", batch_seq, (unsigned)rem, self->stop_requested_.load() ? 1 : 0);
        if (self->stop_requested_) { printf("DD stop_break\n"); break; }


        size_t carry = rem;
        if (carry > 0 && carry < batch_size) {
            memmove(buf, ptr, carry);
        } else {
            carry = 0;
        }


        batch_len = 0; err_cnt = 0; batch_seq++;
        while (batch_len + carry < batch_size && !self->stop_requested_) {
            int r = esp_http_client_read(client, (char*)(buf + carry + batch_len), batch_size - carry - batch_len);
            if (r > 0) { batch_len += r; err_cnt = 0; }
            else if (r == 0) break;
            else { if (++err_cnt > 3) break; vTaskDelay(pdMS_TO_TICKS(50)); }
        }
        total_dl += batch_len;
        if (batch_len == 0) { ESP_LOGI(TAG, "PlayUrl: batch_len=0 break after %dKB", (int)(total_dl/1024)); break; }
        ESP_LOGD(TAG, "PlayUrl: batch#%d dl=%dKB+%uB carry", batch_seq, (int)(batch_len/1024), (unsigned)carry);
        ptr = buf;
        rem = carry + batch_len;

        if (carry == 0 && rem > 3 && !IsValidMpegHeader(ptr)) {
            size_t s = 1;
            while (s + 3 < rem && !IsValidMpegHeader(ptr + s)) s++;
            if (s + 3 < rem) {
                ESP_LOGD(TAG, "PlayUrl: skip %d bytes to next frame", (int)s);
                ptr += s; rem -= s;
            } else {
                rem = 0; // 无效帧头，重�?
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d ready rem=%u %02X%02X%02X%02X...", batch_seq, (unsigned)rem,
               rem>0?ptr[0]:0, rem>1?ptr[1]:0, rem>2?ptr[2]:0, rem>3?ptr[3]:0);
    }
    ESP_LOGD(TAG, "PlayUrl: decode loop done rem=%u stop=%d", (unsigned)rem, self->stop_requested_.load() ? 1 : 0);

cleanup:

    free(buf);
    if (resample_buf1) free(resample_buf1);
    if (resample_buf2) free(resample_buf2);
    esp_mp3_dec_close(dec_handle);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    app.GetAudioService().SetOutputMuted(false);
    self->is_playing_ = false;

    ESP_LOGI(TAG, "PlayUrl: done %dms, dl=%dKB", play_ms, (int)(total_dl/1024));
    vTaskDelete(NULL);
}


/**
 * @brief HTTP PCM 直通播放（无解码，�?CPU 开销�?
 *
 * 通过 HTTP GET 下载原始 PCM�?6kHz/mono/s16le），
 * 分块 push �?PushRawPcmToPlayback 队列�?AudioService 消费�?
 * Ducking: AI 说话时淡出至 20% 音量�?
 *
 * @param url PCM 资源地址
 * @return 0=成功启动
 */
int Mp3Player::PlayPcm(const char* url) {
    if (is_playing_) {
        Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "PlayPcm: launching for %s", url);
    is_playing_ = true;

    struct PcmCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PcmCtx();
    ctx->self = this;
    strncpy(ctx->url, url, 511);
    ctx->url[511] = '\0';

    xTaskCreate(PlayPcmTask, "pcm_http", 8192, ctx, 3, NULL);
    return 0;
}

/**
 * @brief PlayPcmTask �?HTTP PCM 下载+推送任�?
 *
 * HTTP GET �?分块读取�?KB/块）�?Ducking 增益 �?PushRawPcmToPlayback�?
 * 无解码开销，纯 I/O + 乘增益。每~192KB（约4秒）yield 一次防饥饿�?
 *
 * @param arg PcmCtx{self, url} 结构体指�?
 */
void Mp3Player::PlayPcmTask(void* arg) {
    struct PcmCtx { Mp3Player* self; char url[512]; };
    auto* ctx = (PcmCtx*)arg;
    auto* self = ctx->self;
    char url[512];
    strncpy(url, ctx->url, 511);
    url[511] = '\0';
    delete ctx;

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.method = HTTP_METHOD_GET;
    http_cfg.timeout_ms = 15000;
    http_cfg.buffer_size = 4096;

    auto* client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "PlayPcm: client init failed");
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t e = esp_http_client_open(client, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "PlayPcm: open failed %d", e);
        esp_http_client_cleanup(client);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "PlayPcm: HTTP %d, Len=%d", status, content_length);

    if (status != 200) {
        esp_http_client_cleanup(client);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }


const size_t CHUNK = sizeof(self->output_buf_);  // 单次解码缓冲区大�?
    const int SAMPLE_RATE = 24000;
    int64_t t0 = esp_timer_get_time();
    int bytes_yielded = 0;

    while (self->is_playing_ && !self->stop_requested_) {
        int read = esp_http_client_read(client, (char*)self->output_buf_, CHUNK);
        if (read <= 0) break;

size_t samples = read / 2;  // 16位采样数
        int16_t* pcm = (int16_t*)self->output_buf_;

 // ---- Ducking: AI 说话时平滑淡出音�?----
        auto dev_state = app.GetDeviceState();
        if (dev_state == kDeviceStateSpeaking && self->ducking_gain_ >= 1.0f) {
            self->ducking_gain_ = 1.0f;
            self->ducking_start_us_ = esp_timer_get_time();
            ESP_LOGI(TAG, "PlayPcm: AI speaking, fading out");
        }
        if (self->ducking_gain_ < 1.0f) {
            if (dev_state != kDeviceStateSpeaking) {
                self->ducking_gain_ = 1.0f; self->ducking_start_us_ = 0;
            } else {
                int64_t elapsed = esp_timer_get_time() - self->ducking_start_us_;
                self->ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
                if (self->ducking_gain_ < 0.2f) {
                    self->ducking_gain_ = 0.2f;  // hold at 20% as background
                    ESP_LOGI(TAG, "PlayPcm: fade complete, holding at 20%%");
                }
            }
        }

        float g = self->ducking_gain_;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = (int32_t)(pcm[i] * g);
            if (s > 30000) s = 30000;
            if (s < -30000) s = -30000;
            pcm[i] = (int16_t)s;
        }

        app.GetAudioService().PushRawPcmToPlayback(pcm, samples, SAMPLE_RATE);


        bytes_yielded += read;
        if (bytes_yielded >= 192 * 1024) {  // ~4 seconds
            bytes_yielded = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    int64_t play_ms = (esp_timer_get_time() - t0) / 1000;
    self->is_playing_ = false;
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "PlayPcm: done %lldms", play_ms);
    vTaskDelete(NULL);
}

/**
 * @brief OGG/Opus 音乐流播放（BSD Socket + 串口回退双路径）
 *
 * 主路径：BSD socket �?DNS 解析 �?非阻�?connect�?s 超时）→ HTTP GET
 *        �?格式检测：/opus �?OGG 解复�?+ Opus 解码（PushPacketToDecodeQueue�?
 *                    /pcm  �?原始 PCM 直接 PushBackgroundAudio
 * 回退路径：socket 连接失败时走 serial_fallback（stdin fread �?OGG 解复用）
 *
 * Ducking: 主路径在 AI 说话时暂�?TCP 下载（释�?WiFi �?UDP 音频包）�?
 *          回退路径�?AI 说话�?00ms 后停止串口读取�?
 * AEC: /opus 路径输出音量自动降至 65%（减少扬声器回采干扰 AEC 前端）�?
 *
 * @param url Opus/PCM 资源地址
 * @return 0=成功启动, -1=已在进行中或 URL 过长
 */
int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        ESP_LOGW(TAG, "PlayOpus: music already playing, refusing (call stop_music first)");
        return -1;
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
stop_requested_ = false;  // 每次播放前重置停止标�?
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;

    // 清除上次会话残留的背景音频数据，防止开机噪声入�?
    Application::GetInstance().GetAudioService().ClearBackgroundAudio();

    if (strlen(url) >= 512) {
        ESP_LOGE(TAG, "PlayOpus: URL too long (%u bytes)", (unsigned)strlen(url));
        return -1;
    }
    struct OpusCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new OpusCtx();
    ctx->self = this;
    strncpy(ctx->url, url, 511);
    ctx->url[511] = '\0';

    xTaskCreatePinnedToCore(PlayOpusTask, "opus_http", 1024 * 24, ctx, 3, NULL, 1);  // 24KB stack for OGG/Opus decode
    return 0;
}

void Mp3Player::PlayOpusTask(void* arg) {
    struct OpusCtx { Mp3Player* self; char url[512]; };
    auto* ctx = (OpusCtx*)arg;
    auto* self = ctx->self;
    char url[512];
    strncpy(url, ctx->url, 511);
    url[511] = '\0';
    delete ctx;

    ESP_LOGI(TAG, "PlayOpus: task started for %s", url);

    auto& app = Application::GetInstance();

    // ---- 解析 URL：提取主机名、端口、路�?----
    char host[128] = {};
    char path[384] = {};
    int port = 80;
    
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    
    const char* host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t host_len = p - host_start;
    if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
    memcpy(host, host_start, host_len);
    
    if (*p == ':') {
        p++;
        port = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    }
    
    if (*p == '/') {
        strncpy(path, p, sizeof(path) - 1);
    } else {
        path[0] = '/';
    }
    
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    
    // 原始BSD socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "PlayOpus: socket() failed errno=%d", errno);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }
    
    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (!inet_aton(host, &addr.sin_addr)) {
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int ga_ret = getaddrinfo(host, nullptr, &hints, &res);
        if (ga_ret == 0 && res) {
            memcpy(&addr.sin_addr, &((struct sockaddr_in*)res->ai_addr)->sin_addr, 4);
            freeaddrinfo(res);
        } else {
            ESP_LOGE(TAG, "PlayOpus: DNS lookup failed for %s", host);
            close(sock);
            goto serial_fallback;
        }
    }
    
    ESP_LOGI(TAG, "PlayOpus: connecting to %s:%d...", host, port);
    


    {
        int sock_flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, sock_flags | O_NONBLOCK);
        int cret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (cret < 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            struct timeval ct = { .tv_sec = 5, .tv_usec = 0 };
            cret = select(sock + 1, NULL, &wfds, NULL, &ct);
            if (cret <= 0) {
                ESP_LOGE(TAG, "PlayOpus: connect timeout, falling back to serial...");
                close(sock);
                goto serial_fallback;
            }
        } else if (cret < 0) {
            ESP_LOGE(TAG, "PlayOpus: connect() failed errno=%d, falling back to serial...", errno);
            close(sock);
            goto serial_fallback;
        }
        fcntl(sock, F_SETFL, sock_flags);  // restore blocking mode
    }
    
    if (false) {  // gate for serial fallback
serial_fallback:

        
    // === 串口回退模式 ===
        printf("\x01MUSIC_REQ\x02%s\x03\n", url);
        fflush(stdout);
        

        self->stop_requested_ = false;
        self->ducking_gain_ = 1.0f; self->ducking_start_us_ = 0;
        
        
        auto* demuxer = new OggDemuxer();
        demuxer->OnDemuxerFinished([&app](const uint8_t* data, int sample_rate, size_t len) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->timestamp = 0;
            packet->payload.assign(data, data + len);
            app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);
        });
        

        vTaskDelay(pdMS_TO_TICKS(3000));  // Wait 3s for serial_relay to fetch data
        
        uint8_t serial_buf[4096];
        uint64_t serial_start = esp_timer_get_time() / 1000;
        uint64_t last_data_ms = serial_start;
        uint64_t last_refresh_ms = serial_start;
        size_t total_dl = 0;
        bool stream_started = false;
        bool ducked_serial = false;
        uint64_t ducked_serial_at = 0;
        
        while (self->is_playing_ && !self->stop_requested_) {
            size_t n = fread(serial_buf, 1, sizeof(serial_buf), stdin);
            if (n > 0) {
                stream_started = true;
                last_data_ms = esp_timer_get_time() / 1000;
                total_dl += n;
                demuxer->Process(serial_buf, n);

                uint64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_refresh_ms > 8000) {  // every 8s
        // HACK: 长时间下载时防止音频看门狗超�?
                app.GetAudioService().RefreshInputTimestamp();

                    last_refresh_ms = now_ms;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            uint64_t now = esp_timer_get_time() / 1000;
            if (stream_started && (now - last_data_ms > 5000)) break;  // 5s idle
            if (!stream_started && (now - serial_start > 15000)) break;  // 15s startup
            if (now - serial_start > 120000) break;  // 2min total
            if (app.GetDeviceState() == kDeviceStateConnecting) break;  // 断连词唤醒后，停止下�?

            auto serial_state = app.GetDeviceState();
            if (serial_state == kDeviceStateSpeaking) {
                if (!ducked_serial) {
                    ducked_serial = true;
                    ducked_serial_at = now;
                    continue;
                } else if (now - ducked_serial_at >= 500) {
                    ESP_LOGI(TAG, "PlayOpus: serial voice stop (gap %llums)", now - ducked_serial_at);
                    break;
                }
            } else {
                ducked_serial = false;
            }
        }
        
        app.GetAudioService().WaitForPlaybackQueueEmpty();
        app.GetAudioService().ResetDecoder();
        delete demuxer;
        self->is_playing_ = false;
        
        int64_t total_ms = (esp_timer_get_time() / 1000) - serial_start;
        ESP_LOGI(TAG, "PlayOpus: serial done %dms, dl=%dKB", (int)total_ms, (int)(total_dl / 1024));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "PlayOpus: connected to %s:%d", host, port);
    

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    send(sock, req, strlen(req), 0);
    

    char header_buf[1024] = {};
    int hdr_pos = 0;
    while (hdr_pos < 1023) {
        int r = recv(sock, header_buf + hdr_pos, 1, 0);
        if (r <= 0) break;
        hdr_pos += r;
        if (hdr_pos >= 4 && memcmp(header_buf + hdr_pos - 4, "\r\n\r\n", 4) == 0) break;
    }
    header_buf[hdr_pos] = '\0';
    
    int status = 0;
    sscanf(header_buf, "HTTP/1.%*d %d", &status);
    ESP_LOGI(TAG, "PlayOpus: HTTP %d", status);
    
    if (status != 200) {
        ESP_LOGE(TAG, "PlayOpus: HTTP error %d", status);
        close(sock);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }

    // === 格式检�? /opus �?OGG 解复用器 + 主音频管�?===
    // === /pcm 走原始字节推送背景环形缓冲区 ===
    bool use_opus = (strstr(path, "/opus") != nullptr);
    
    if (use_opus) {
        // =========== Opus 路径: OGG 解复�?+ PushPacketToDecodeQueue ============

        ESP_LOGI(TAG, "PlayOpus: downloading Opus OGG...");
        
        auto* demuxer = new OggDemuxer();
        demuxer->OnDemuxerFinished([&app, self](const uint8_t* data, int sample_rate, size_t len) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->timestamp = 0;
            packet->payload.assign(data, data + len);
            app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);
        });
        
        const size_t CHUNK = 4096;
        uint8_t buf[CHUNK];
        int64_t t0 = esp_timer_get_time(), last_refresh = t0;
        size_t total_dl = 0;
        

        auto* codec = Board::GetInstance().GetAudioCodec();
        int old_vol = codec ? codec->output_volume() : 90;
        int music_vol = old_vol * 65 / 100;
        if (music_vol < 10) music_vol = 10;
        if (codec) codec->SetOutputVolume(music_vol);
        ESP_LOGI(TAG, "PlayOpus: volume %d -> %d for AEC", old_vol, music_vol);
        
        bool is_ducking = false;  // true = AI speaking, don't push music
        
        while (self->is_playing_ && !self->stop_requested_) {
            int read = recv(sock, buf, CHUNK, 0);
            if (read <= 0) {
                ESP_LOGW(TAG, "PlayOpus: recv returned %d errno=%d", read, (read < 0) ? errno : 0);
                break;
            }
            total_dl += read;





            auto dev_state = app.GetDeviceState();
            if (dev_state == kDeviceStateConnecting) {
                ESP_LOGI(TAG, "PlayOpus: auto-stop (wake word)");
                break;
            }
            if (dev_state == kDeviceStateSpeaking) {
                if (!is_ducking) {
                    is_ducking = true;
                    ESP_LOGI(TAG, "PlayOpus: ducking for AI speech...");
                }
                demuxer->Process(buf, read);  // keep decoder state, skip output
                continue;
            }
            if (is_ducking) {
                ESP_LOGI(TAG, "PlayOpus: resuming music");
                is_ducking = false;
            }

            demuxer->Process(buf, read);

            int64_t now = esp_timer_get_time();
            if (now - last_refresh > 8000000) {
                app.GetAudioService().RefreshInputTimestamp();
                last_refresh = now;
            }
        }
        

        if (codec) codec->SetOutputVolume(old_vol);
        ESP_LOGI(TAG, "PlayOpus: volume restored to %d", old_vol);
        

        vTaskDelay(pdMS_TO_TICKS(1000));
        app.GetAudioService().WaitForPlaybackQueueEmpty();
        
        delete demuxer;
        close(sock);
        self->is_playing_ = false;
        int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "PlayOpus: done %dms, dl=%dKB (Opus)", (int)total_ms, (int)(total_dl / 1024));
        vTaskDelete(NULL);
        return;
    }
    
    // ============ PCM 路径 (�?: 原始 s16le PushBackgroundAudio ============
    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    int64_t t0 = esp_timer_get_time(), last_refresh = t0;
    size_t total_dl = 0;
    bool ai_speaking = false;

    app.GetAudioService().SetBackgroundAudioGain(0.001f);  // near-silent but keeps bg_audio_active_=true
    ESP_LOGI(TAG, "PlayOpus: downloading raw PCM...");

    // === 预填充阶�? 先填满环形缓冲区再开启消�?===
    int64_t prebuf_start = esp_timer_get_time();
    while (self->is_playing_ && !self->stop_requested_) {
        int read = recv(sock, buf, CHUNK, 0);
        if (read <= 0) break;
        total_dl += read;

        app.GetAudioService().PushBackgroundAudio(
            reinterpret_cast<int16_t*>(buf), read / sizeof(int16_t), 16000);

        size_t fill = app.GetAudioService().GetBgAudioFillLevel();
        if (fill >= 64000) {
        // 开启消耗前根据当前 AI 状态设置增�?
        // (防止满量音乐 + TTS 重叠噪声)
            auto state = app.GetDeviceState();
            // 仅在 AI 说话�?Ducking; 监听保持满音�?(用户 2026/7/20 要求)
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGI(TAG, "PlayOpus: drain enabled (buffered %d samples in %d ms)",
                     (int)fill, (int)((esp_timer_get_time() - prebuf_start) / 1000));
            break;
        }
        if (esp_timer_get_time() - prebuf_start > 10000000) {
        // 超时也要设置增益，否则会停留�?0.001f
            auto state = app.GetDeviceState();
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGW(TAG, "PlayOpus: pre-buffer timeout after 10s (%d samples buffered)",
                     (int)fill);
            break;
        }
    }

    // === 主下载推送循�?===
    int recv_errors = 0;
    while (self->is_playing_ && !self->stop_requested_) {
        auto state = app.GetDeviceState();
            // 监听时保�?00% 音量; �?AI 说话/连接时降�?0%
        bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
        if (ai_now != ai_speaking) {
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            int heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            int bg_fill = app.GetAudioService().GetBgAudioFillLevel();
            int task_hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "PlayOpus: AI %s, music -> %d%%, heap=%d sram=%d bg_buf=%d task_hwm=%d",
                     ai_now ? "speaking" : "quiet", ai_now ? 50 : 100,
                     heap_free / 1024, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     bg_fill, task_hwm);
        }

            // AI 说话时暂�?TCP 下载，释�?WiFi 空中时间�?
            // UDP 音频�?(防止 WiFi 缓冲区饥饿导�?TTS 卡顿)
            // 仅在缓冲区足够支撑典�?AI 回复时长时暂�?
        if (ai_speaking && app.GetAudioService().GetBgAudioFillLevel() > 64000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int read = recv(sock, buf, CHUNK, 0);
        if (read > 0) {
            total_dl += read;
            recv_errors = 0;
            app.GetAudioService().PushBackgroundAudio(
                reinterpret_cast<int16_t*>(buf), read / sizeof(int16_t), 16000);
        } else if (read == 0) {
            // 服务器优雅关闭连�?
            ESP_LOGI(TAG, "PlayOpus: server closed connection");
            break;
        } else {
            // read < 0: 超时或瞬时错�?
            recv_errors++;
            if (recv_errors > 5) {
                ESP_LOGE(TAG, "PlayOpus: recv failed %d times, giving up (errno=%d)", recv_errors, errno);
                break;
            }
            // 等待1s 后重�?(WiFi 可能�?AI 对话后重连中)
            ESP_LOGW(TAG, "PlayOpus: recv error %d/%d (errno=%d), retrying...", recv_errors, 5, errno);
            for (int w = 0; w < 10 && self->is_playing_ && !self->stop_requested_; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            // 刷新省电设置 (通道关闭可能改变了它)
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            continue;
        }

        while (app.GetAudioService().GetBgAudioFillLevel() > 80000 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_refresh > 8000000) {
            // 诊断日志: �?s 记录缓冲区填�?+ 下载速率
            size_t fill = app.GetAudioService().GetBgAudioFillLevel();
            int64_t elapsed = now - t0;
            float dl_rate = elapsed > 0 ? (float)total_dl * 1000000.0f / (float)elapsed : 0;
            bool duck_now = (app.GetDeviceState() == kDeviceStateSpeaking || 
                            app.GetDeviceState() == kDeviceStateConnecting);
            int heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            int sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            int task_hwm = uxTaskGetStackHighWaterMark(NULL);
            bool aec_active = true;
            ESP_LOGI(TAG, "PlayOpus: buf=%d dl=%dKB rate=%.1fKB/s duck=%d%% heap=%dKB sram=%d task_hwm=%d aec=%d",
                     (int)fill, (int)(total_dl / 1024), dl_rate / 1024.0f, duck_now ? 50 : 100,
                     heap_free / 1024, sram_free, task_hwm, aec_active ? 1 : 0);
            app.GetAudioService().RefreshInputTimestamp();
            last_refresh = now;
        }
    }

    if (total_dl > 0) {
        app.GetAudioService().SetBackgroundAudioGain(1.0f);
        while (app.GetAudioService().GetBgAudioFillLevel() > 0 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    app.GetAudioService().ClearBackgroundAudio();
    close(sock);
    self->is_playing_ = false;
    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "PlayOpus: done %dms, dl=%dKB", (int)total_ms, (int)(total_dl / 1024));





    if (app.GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "PlayOpus: restarting wake word detection after music");
        app.GetAudioService().EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        app.GetAudioService().EnableWakeWordDetection(true);
        // Fix(2026-07-19): 音乐期间 idle 切换会跳过阈值恢�?
        // (IsBgAudioActive 守卫) 会卡�?.30。在此处恢复敏感�?.02
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 播放闹钟铃声（带渐强效果�?
 *
 * @param volume 音量系数 0.0~1.0，开头渐强效果（15%�?00%�?
 */
void Mp3Player::PlayAlarmRing(float volume) {
    auto& app = Application::GetInstance();
    // ---- 闹铃参数 ----
    const int sample_rate = 16000;     // 16kHz 采样率（足够的音频质量）
    const int chunk_ms = 100;          // 100ms 分块以便快速响应停止请�?
    const int total_ms = 2000;         // 总共 2 �?
    const int chunk_samples = sample_rate * chunk_ms / 1000;  // 每块 1600 采样
    const int total_chunks = total_ms / chunk_ms;              // �?20 �?

    // ---- 音调和节�?----
    const int freq_a = 880;            // A5 音符
    const int freq_b = 1100;           // C#6 音符（两个音调交替）
    const int base_amplitude = 8000;   // 基础振幅（int16 满幅 32767 �?~24%�?
    const int beep_on_ms = 80;         // 发声 80ms
    const int beep_off_ms = 80;        // 静音 80ms
    const int cycle_ms = beep_on_ms + beep_off_ms;  // 完整周期 160ms

    // 闹铃节奏：chunk(100ms) 内按 beep_on/beep_off 切换音调和静�?
    // 同时整体音量�?15% 线性升�?100%


    size_t buf_bytes = chunk_samples * sizeof(int16_t);
    int16_t* pcm = (int16_t*)malloc(buf_bytes);
    if (!pcm) {
        ESP_LOGE(TAG, "Alarm: failed to allocate PCM buffer");
        return;
    }

    ESP_LOGI(TAG, "Playing alarm ring: PCM beep %d/%dHz, %dms in %dms chunks",
             freq_a, freq_b, total_ms, chunk_ms);

    for (int chunk = 0; chunk < total_chunks && !stop_requested_; chunk++) {
        int sample_offset = chunk * chunk_samples;
        for (int i = 0; i < chunk_samples; i++) {
            int global_i = sample_offset + i;
            int ms = global_i * 1000 / sample_rate;
            int cycle = ms / cycle_ms;
            int phase = ms % cycle_ms;
            if (phase < beep_on_ms) {
                int freq = (cycle % 2 == 0) ? freq_a : freq_b;
                double t = (double)global_i / sample_rate;
                int16_t val = (int16_t)(sin(2.0 * M_PI * freq * t) * base_amplitude * volume);
                pcm[i] = val;
            } else {
                pcm[i] = 0;
            }
        }
        app.GetAudioService().OutputRawPcm(pcm, chunk_samples, sample_rate);
        vTaskDelay(pdMS_TO_TICKS(chunk_ms));
    }

    free(pcm);
    ESP_LOGI(TAG, "Alarm ring finished (stopped=%d)", stop_requested_.load() ? 1 : 0);
}

/**
 * @brief 播放调度任务（FreeRTOS 持久化任务，�?50ms tick 一次）
 *
 * 轮询 pending_track_（单曲播放）�?pending_bell_hour_（钟声播放）�?
 * 仅负责调度，具体播放由各异步 task（PlayUrlTask / PlayOpuTask / PlayPcmTask）完成�?
 * 每次播放�?SetOutputMuted(true) 防止音频冲突，播放后恢复�?
 */

void Mp3Player::PlayTaskEntry(void* arg) {
    Mp3Player* self = (Mp3Player*)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (self->stop_requested_) {
            self->is_playing_ = false;
            self->stop_requested_ = false;
            self->ducking_gain_ = 1.0f; self->ducking_start_us_ = 0;
            continue;
        }

        if (self->pending_track_ > 0) {
    // 播放单曲 - 在此处管理静�?
            self->is_playing_ = true;
            int track = self->pending_track_;
            self->pending_track_ = 0;
            if (!self->disable_ducking_) {
                Application::GetInstance().GetAudioService().SetOutputMuted(true);
            }
            self->DecodeSingleFile(track);
            self->is_playing_ = false;
            if (!self->disable_ducking_) {
                Application::GetInstance().GetAudioService().SetOutputMuted(false);
            }
        }

        if (self->pending_bell_hour_ > 0) {
            // 播放钟声 (�?PCM 铃声, 每次~0.5s)
            self->is_playing_ = true;
            int hour = self->pending_bell_hour_;
            self->pending_bell_hour_ = 0;
            Application::GetInstance().GetAudioService().SetOutputMuted(true);
            for (int i = 0; i < hour; i++) {
                if (self->stop_requested_) break;
                self->bell_player_.PlayBellSoundSync();
            // 每次敲击间隔~1s, 模拟自然钟声
                if (i < hour - 1 && !self->stop_requested_) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            self->is_playing_ = false;
            Application::GetInstance().GetAudioService().SetOutputMuted(false);
        }
    }
}

/**
 * @brief 文件�?曲目号播放入口（TF 卡模式兼容接口）
 *
 * @param folder 1=钟声, 2=歌曲目录
 * @param track  文件夹内曲目号（钟声小时�? 歌曲 1-12�?
 */
void Mp3Player::PlayTrack(uint8_t folder, uint8_t track) {
    if (folder == 1) {

        PlayBell(track);
    } else if (folder == 2) {

        if (track < 1) track = 1;
        if (track > 12) track = 12;
        PlayIndex(track);
    }
}

/**
 * @brief 按编号播�?assets 分区中的本地 MP3
 *
 * �?Stop() 清场，再设置 pending_track_ 通知 PlayTaskEntry�?
 * �?play_task_ 不存在则创建 Core 1 持久化播放调度任务（stack 4KB）�?
 *
 * @param index MP3 文件编号�?-16�?
 */
void Mp3Player::PlayIndex(uint16_t index) {
    if (index < 1) index = 1;

    if (!assets_) {
        ESP_LOGE(TAG, "Mp3Player not initialized (call Init first)");
        return;
    }


    Stop();


    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;


    if (!play_task_) {
        stop_requested_ = false;
        ducking_gain_ = 1.0f; ducking_start_us_ = 0;
        xTaskCreatePinnedToCore(
            PlayTaskEntry,
            "mp3_play",
            4096,
            this,
            5,
            &play_task_,
            1
        );
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief 停止当前播放
 *
 * 设置 stop_requested_ 标志位，各异步任务轮询后自行退出�?
 * 清空 pending_track_ / pending_bell_hour_，resete MP3 decoder�?
 */
void Mp3Player::Stop() {
    stop_requested_ = true;


    pending_track_ = 0;
    pending_bell_hour_ = 0;

    if (mp3_dec_handle_) {
        esp_mp3_dec_reset(mp3_dec_handle_);
    }
    

    Application::GetInstance().GetAudioService().ResetDecoder();
}

void Mp3Player::Pause() {
    stop_requested_ = true;
    is_playing_ = false;
}

/**
 * @brief 重置播放状态（新播放前调用�?
 *
 * �?Stop 更轻量：仅重置标志位不清�?buffer�?
 */
void Mp3Player::ResetForNextPlay() {
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    is_playing_ = false;
    pending_track_ = 0;
    pending_bell_hour_ = 0;
}

// ---- 播放控制桩函数（暂停/恢复/音量/切歌�?---
// 当前未实现完整功能，预留接口�?MCP 工具调用
void Mp3Player::Resume() {
    ESP_LOGW(TAG, "Resume not supported, call PlayIndex again");
}

void Mp3Player::SetVolume(uint8_t vol) {
    ESP_LOGI(TAG, "Volume request: %d (handled by AudioService)", vol);
}

void Mp3Player::VolumeUp() { SetVolume(20); }
void Mp3Player::VolumeDown() { SetVolume(10); }
void Mp3Player::Next() {}
void Mp3Player::Prev() {}

/**
 * @brief 播放报时钟声�?-12 声，每声间隔 ~1s�?
 *
 * 通过 pending_bell_hour_ 通知 PlayTaskEntry 在异步任务中执行�?
 * 每声 ~0.5s PCM 铃声，双声间�?1s 模拟自然钟声节奏�?
 *
 * @param hour 当前小时数（1-12 小时制，超范围自动钳位）
 */
void Mp3Player::PlayBell(int hour) {
    // 时钟制式�?-12 小时制，超范围钳�?
    if (hour < 1) hour = 1;
    if (hour > 12) hour = 12;

    if (!assets_) return;

    // 停止当前播放，为新报时清�?
    Stop();

    // 记录报时点数，异步任务中读取
    pending_bell_hour_ = hour;


    if (!play_task_) {
        stop_requested_ = false;
        ducking_gain_ = 1.0f; ducking_start_us_ = 0;
        xTaskCreatePinnedToCore(
            PlayTaskEntry,
            "mp3_play",
            4096,
            this,
            5,
            &play_task_,
            1
        );
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief 播放背景音乐（兼容接口，等同�?PlayIndex�?
 *
 * @param index MP3 文件编号（≥1�?
 */
void Mp3Player::PlayBgMusic(int index) {
    if (index < 1) index = 1;
    PlayIndex(index);
}

/**
 * @brief MP3 解码到内存缓冲区（不播放，供背景音频管线使用�?
 *
 * 完整解码指定 MP3 文件到动态分配的 SPIRAM 缓冲区�?
 * 调用方通过 out_buf/out_samples/out_samplerate 获取结果并负�?heap_caps_free�?
 * 跳过 ID3v2 标签，缓冲区上限 2M samples（约 4MB）�?
 *
 * @param index          MP3 文件编号
 * @param[out] out_buf   解码�?PCM 缓冲区指针（SPIRAM，调用方释放�?
 * @param[out] out_samples 输出采样�?
 * @param[out] out_samplerate 输出采样率（�?MP3 头检测）
 * @return 0=成功, -1=失败
 */
int Mp3Player::DecodeToBuffer(int index, int16_t** out_buf, size_t* out_samples, int* out_samplerate) {
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

    void* mp3_data = nullptr;
    size_t mp3_size = 0;
    if (!assets_->GetAssetData(filename, mp3_data, mp3_size)) {
        ESP_LOGE(TAG, "Failed to get asset: %s", filename);
        return -1;
    }
    if (!mp3_data || mp3_size == 0) {
        ESP_LOGE(TAG, "Empty asset: %s", filename);
        return -1;
    }


    size_t max_samples = mp3_size * 4;
    if (max_samples < 8192) max_samples = 8192;
    if (max_samples > 2 * 1024 * 1024) max_samples = 2 * 1024 * 1024;  // cap at 2M samples (4MB)
    int16_t* buf = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu samples in PSRAM", (unsigned long)max_samples);
        return -1;
    }


    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }
    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        heap_caps_free(buf);
        return -1;
    }

    // 跳过 ID3v2 标签
    uint8_t* mp3_start = (uint8_t*)mp3_data;
    size_t mp3_data_size = mp3_size;
    if (mp3_size > 10 && memcmp(mp3_start, "ID3", 3) == 0) {
        uint32_t id3_size = ((mp3_start[6] & 0x7F) << 21) | ((mp3_start[7] & 0x7F) << 14)
                          | ((mp3_start[8] & 0x7F) << 7)  |  (mp3_start[9] & 0x7F);
        size_t skip = 10 + id3_size;
        if (skip < mp3_size - 1024) {
            mp3_start += skip;
            mp3_data_size -= skip;
        }
    }

    esp_audio_dec_info_t dec_info = {};
    int sample_rate = 22050;
    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;
    size_t written = 0;


    while (remaining > 0 && written < max_samples) {
        size_t in_len = (remaining < kInputBufSize) ? remaining : kInputBufSize;
        memcpy(input_buf_, input_ptr, in_len);

        esp_audio_dec_in_raw_t raw;
        raw.buffer = input_buf_;
        raw.len = in_len;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        esp_audio_dec_out_frame_t frame;
        frame.buffer = output_buf_;
        frame.len = kOutputBufSize;
        frame.needed_size = 0;
        frame.decoded_size = 0;

        ret = esp_mp3_dec_decode(mp3_dec_handle_, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK) {
            if (sample_rate == 22050 && dec_info.sample_rate) {
                sample_rate = dec_info.sample_rate;
            }
            if (frame.decoded_size > 0) {
                size_t n = frame.decoded_size / sizeof(int16_t);
                if (written + n <= max_samples) {
                    memcpy(buf + written, frame.buffer, frame.decoded_size);
                    written += n;
                }
            }
            size_t consumed = raw.consumed;
            if (consumed == 0) {
                // 解码器已缓冲全部输入; 无法继续, 停止
                break;
            }
            if (consumed > remaining) consumed = remaining;
            input_ptr += consumed;
            remaining -= consumed;
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            break;
        } else {
            if (raw.consumed) {
                size_t skip = raw.consumed;
                if (skip > remaining) skip = remaining;
                input_ptr += skip;
                remaining -= skip;
            } else {
                break;
            }
        }
    }


    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    if (written == 0) {
        ESP_LOGE(TAG, "No PCM data decoded from %s", filename);
        heap_caps_free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_samples = written;
    *out_samplerate = sample_rate;
    ESP_LOGI(TAG, "Decoded %s to PCM: %u samples, %d Hz", filename, (unsigned)written, sample_rate);
    return 0;
}

// ============================================

// ============================================// ===== Part 3: LdrSensor、BellSoundPlayer、Dance、NVS 存储 (L1925-2394 / 原始行号) =====
// ============================================
// Part 3 包含以下模块�?
// - LdrSensor: 光敏电阻 ADC 读取，昼夜检测（配置�?config.h LDR_ADC_*�?
// - BellSoundPlayer: 布谷鸟唤醒音 / 报时钟声播放（同�?异步�?
// - CuckooStateMachine 构�?析构 + 电机电源 + LED 初始�?
// - 舞蹈序列：RunDanceIntro �?RunDanceLoop �?RunDanceFinale
// - NVS 闹钟持久化：SaveAlarmsToNvs / LoadAlarmsFromNvs
// ============================================
/**
 * @brief 光敏电阻传感器构造函�?
 *
 * 配置 ADC oneshot 单元用于连续采样光敏电阻分压值�?
 * 12-bit 分辨率（0~4095），值越�?越暗�?
 *
 * @param adc_pin   ADC 输入 GPIO 引脚
 * @param unit      ADC 单元编号（ADC_UNIT_1 �?ADC_UNIT_2�?
 * @param chan      ADC 通道�?
 * @param threshold 暗光判定阈值（默认 2000，低于此值判为黑暗）
 */
LdrSensor::LdrSensor(gpio_num_t adc_pin, adc_unit_t unit, adc_channel_t chan, int threshold)
    : adc_pin_(adc_pin), adc_handle_(nullptr), adc_chan_(chan), threshold_(threshold) {

 // ---- ADC oneshot init ----
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle_));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = LDR_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_chan_, &chan_cfg));

    ESP_LOGI(TAG, "LDR sensor on GPIO %d (ADC%d_CH%d), threshold %d",
             adc_pin_, (int)unit, (int)chan, threshold_);
}

LdrSensor::~LdrSensor() {
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }
}

/**
 * @brief 读取光敏电阻 ADC 原始值（12-bit�?~4095�?
 * @return 原始 ADC 值，值越�?越暗
 */
int LdrSensor::ReadRaw() {
    int raw = 0;
    if (adc_handle_) {
        adc_oneshot_read(adc_handle_, adc_chan_, &raw);
    }
    return raw;
        // raw: 0~4095 (12-bit ADC)，数值越�?越暗
}

/**
 * @brief 判断当前是否为黑暗环�?
 * @return true=黑暗（ADC �?< 阈值），false=明亮
 */
bool LdrSensor::IsDark() { return ReadRaw() < threshold_; }

/**
 * @brief 动态设置暗光判定阈�?
 * @param threshold 新阈值（0~4095�?
 */void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }

// ============================================
// BellSoundPlayer �?布谷鸟唤醒音 / 报时钟声播放
// ============================================
/**
 * @brief 同步播放布谷鸟唤醒音（阻塞，直接写入音频输出�?
 *
 * 播放预编译的 cuckoo_wake_sound PCM 数据�?
 * 输出期间阻塞调用方，�?1-2 秒�?
 */
void BellSoundPlayer::PlayCuckooSoundSync() {

    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_wake_sound,
        CUCKOO_WAKE_SOUND_NUM_SAMPLES,
        CUCKOO_WAKE_SOUND_SAMPLE_RATE
    );
}

/**
 * @brief 同步播放报时钟声（阻塞，单次敲击 ~0.5s�?
 *
 * 播放预编译的 cuckoo_bell_sound PCM 数据�?
 * 通常�?PlayTaskEntry 循环调用（每小时 1-12 次）�?
 */
void BellSoundPlayer::PlayBellSoundSync() {

    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_bell_sound,
        CUCKOO_BELL_SOUND_NUM_SAMPLES,
        CUCKOO_BELL_SOUND_SAMPLE_RATE
    );
}

/**
 * @brief 异步播放布谷鸟唤醒音（不阻塞调用方）
 *
 * spawn 独立 FreeRTOS task（stack 2KB）执行同步播放后自动删除�?
 */
void BellSoundPlayer::PlayCuckooSoundAsync() {
    xTaskCreate([](void* arg) {
        auto* self = static_cast<BellSoundPlayer*>(arg);
        self->PlayCuckooSoundSync();
        vTaskDelete(NULL);
    }, "cuckoo_async", 2048, this, 5, NULL);
}

// ============================================

/**
 * @brief 布谷鸟钟状态机构造函�?
 *
 * 接收所有硬件依赖注入（电机、舵机、音频、传感器），初始化：
 * - 电机电源 P-MOSFET 控制（GPIO LOW=ON�?
 * - LED 双路指示灯（S8050 NPN 驱动，GPIO �?亮）
 * - 所有表演状态变量归�?
 *
 * @param m1          舞蹈电机 M1
 * @param m2          大门电机 M2
 * @param m3          小狗电机 M3
 * @param m4          小鸟门电�?M4
 * @param violin_motor 小提琴升降电�?
 * @param violin      小提琴手臂舵�?
 * @param dog         狗尾舵机
 * @param water_bird  水车+鸟跳电机
 * @param mp3         Mp3Player 实例
 * @param bell_player BellSoundPlayer 实例
 * @param ldr         LdrSensor 实例
 */
CuckooStateMachine::CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                                        Motor* violin_motor, Servo* violin, Servo* dog,
                                        Motor* water_bird, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
      violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),
            // 小提琴舵机（IN B M2, GPIO18/45�?
      water_bird_(water_bird),
      mp3_(mp3), bell_player_(bell_player), ldr_(ldr),
      motor_power_pin_(GPIO_NUM_NC),
      current_performance_(kPerformanceNone), current_phase_(kPhaseIdle),
      call_count_(0), total_calls_(0), is_running_(false),
      last_hour_(-1), last_half_hour_(-1), show_music_index_(0),
      current_hour_(12), current_min_(0), current_sec_(0), time_set_(false),
      is_dark_(false) {
    last_idle_exit_us_ = 0;
    prev_device_state_ = -1;

    // 初始化电机电�?P-MOSFET 控制 (GPIO LOW=ON 导�? HIGH=OFF 断开)
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);

    // 初始�?LED 双路指示�?(S8050 NPN 驱动, GPIO �?�?
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_A_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_cfg);
        // LED 指示�?(S8050 NPN 驱动, 5V 供电)
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    MotorPowerOff();
  }

CuckooStateMachine::~CuckooStateMachine() {
    StopAll();
    MotorPowerOff();
}

/**
 * @brief 打开电机电源（P-MOSFET 导通→5V 供电�?
 */
void CuckooStateMachine::MotorPowerOn() {
    gpio_set_level(motor_power_pin_, 0);
}

/**
 * @brief 关闭电机电源（P-MOSFET 截止�?V 断开�?
 */
void CuckooStateMachine::MotorPowerOff() {
    gpio_set_level(motor_power_pin_, 1);
}

// ============================================
// 开门异步任�?
// ============================================

/** @brief 异步开门任务上下文（堆分配，任务内 delete�?*/
struct DoorOpenCtx {
    CuckooStateMachine* sm;
};

/**
 * @brief 异步开门任�?
 *
 * 独立 FreeRTOS task（stack 2KB，Core 1）：
 * 上电 �?M2 正向开门（MAIN_DOOR_TIME_MS）→ 停止 �?退�?
 *
 * @param arg DoorOpenCtx{sm} 结构体指�?
 */
void CuckooStateMachine::DoorOpenTask(void* arg) {
    auto* ctx = static_cast<DoorOpenCtx*>(arg);
    auto* sm = ctx->sm;
    delete ctx;

    sm->MotorPowerOn();
    if (sm->m2_) {
        sm->m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        sm->m2_->Stop();
    }
    ESP_LOGI(TAG, "Door open done (async)");
    vTaskDelete(NULL);
}

/**
 * @brief 播放狗叫声（WAV 解码 + 自适应路由�?
 *
 * �?assets 分区读取 dog_bark.wav，解�?WAV 头（仅支�?mono s16）�?
 * 路由策略�?
 * - 背景音频活跃 �?MixIntoBackgroundAudio（叠加混音，gain 0.9，不打断音乐�?
 * - 背景音频未激�?�?OutputRawPcm（直接输出，�?DogShow 场景�?
 */
void CuckooStateMachine::PlayDogBark() {

    const char* filename = "dog_bark.wav";
    void* wav_ptr = nullptr;
    size_t wav_size = 0;
    if (!Assets::GetInstance().GetAssetData(filename, wav_ptr, wav_size)) {
        ESP_LOGW(TAG, "PlayDogBark: %s not found", filename);
        return;
    }
    if (wav_size < 44) {
        ESP_LOGW(TAG, "PlayDogBark: file too small (%u bytes)", (unsigned)wav_size);
        return;
    }

    const uint8_t* wav_data = (const uint8_t*)wav_ptr;
    uint16_t channels = wav_data[22] | (wav_data[23] << 8);
    uint32_t sample_rate = wav_data[24] | (wav_data[25] << 8) | (wav_data[26] << 16) | (wav_data[27] << 24);
    uint16_t bits = wav_data[34] | (wav_data[35] << 8);
    ESP_LOGI(TAG, "DogBark: %u bytes, %d Hz, %d ch, %d-bit", (unsigned)wav_size, sample_rate, channels, bits);

    if (bits != 16 || channels != 1) {
        ESP_LOGW(TAG, "PlayDogBark: need mono s16, got %dch %d-bit", channels, bits);
        return;
    }


    const int16_t* pcm = (const int16_t*)(wav_data + 44);
    size_t pcm_bytes = wav_size - 44;
    size_t num_samples = pcm_bytes / sizeof(int16_t);

    auto& app = Application::GetInstance();
    if (app.GetAudioService().IsBgAudioActive()) {
        app.GetAudioService().MixIntoBackgroundAudio(pcm, num_samples, 0.9f);
        ESP_LOGI(TAG, "DogBark: mixed %u samples into bg audio", (unsigned)num_samples);
    } else {
        app.GetAudioService().OutputRawPcm(pcm, num_samples, sample_rate);
        int play_ms = (int)(num_samples * 1000 / sample_rate);
        vTaskDelay(pdMS_TO_TICKS(play_ms + 100));
        ESP_LOGI(TAG, "DogBark: played %u samples via OutputRawPcm", (unsigned)num_samples);
    }
}

/**
 * @brief 舞蹈开场动作序�?
 *
 * 并行执行�?
 * - 异步开门任务（DoorOpenTask, Core 1�?
 * - 狗尾舵机扫出 180°�?0°
 * - 小狗前进（DOG_WALK_TIME_MS�?
 * - 狗叫一�?
 * - 狗尾扫至 0° 归位
 *
 * �?ShowTask / PerformanceTask 中调用�?
 */
void CuckooStateMachine::RunDanceIntro() {

    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open", 2048, ctx, 5, nullptr, 1);


    if (dog_servo_) {
            // 小狗尾巴�?80°�?0° 扫出，准备前�?
        dog_servo_->Sweep(180, 20, (180 - 20) * 15);
    }

    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }
    PlayDogBark();

    if (dog_servo_) {
            // 小狗进门后摇尾归�?
        dog_servo_->Sweep(20, 0, 20 * 15);
    }
}

/**
 * @brief 舞蹈主循环（20fps，最�?120s�?
 *
 * 等待背景音频激活（最�?5s）后进入循环，每�?50ms�?
 * - M1 舞蹈电机：正�?反转随机交替�?~3s 随机时长 ×2 方向�?
 * - 小提琴：10 段预设动作循环（左右摆臂+升降拉琴�?
 * - 狗尾：随机方�?频率摇尾�?~60° 范围�?
 * - LED：双路交替闪烁（�?6 �?300ms 翻转�?
 * - 水车：持续旋�?
 *
 * 退出条件：背景音频停止 �?旧音乐路径也停止，或 120s 超时�?
 */
void CuckooStateMachine::RunDanceLoop() {
    unsigned long m1_timer = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    int64_t next_frame_us = esp_timer_get_time();
    m1_fwd_time_ = 0;
    m1_rev_time_ = 0;
    m1_stage_start_ = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 等待背景音乐启动（最�?5s），避免循环立刻退�?
    for (int w = 0; w < 100 && is_running_
        && !Application::GetInstance().GetAudioService().IsBgAudioActive()
        && (!mp3_ || !mp3_->IsPlaying()); w++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    unsigned long dance_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (is_running_ && (Application::GetInstance().GetAudioService().IsBgAudioActive()
           || (mp3_ && mp3_->IsPlaying()))  // also covers old PlayBgMusic path
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - dance_start_ms) < 120000UL) {  // 120s safety timeout
        if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;


        switch (m1_stage) {
            case 0:
                if (m1_) m1_->Forward(DANCE_SPEED_PERCENT);
                if (now - m1_timer > m1_rand_time) {
                    m1_fwd_time_ += (now - m1_stage_start_);
                    m1_stage_start_ = now;
                    m1_timer = now; m1_stage = 1;
                }
                break;
            case 1:
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 2;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
            case 2:
                if (m1_) m1_->Reverse(DANCE_SPEED_PERCENT);
                if (now - m1_timer > m1_rand_time) {
                    m1_rev_time_ += (now - m1_stage_start_);
                    m1_stage_start_ = now;
                    m1_timer = now; m1_stage = 3;
                }
                break;
            case 3:
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }

        // 小提�?10 段循环动作：左右摆臂 + 升降拉琴交替
        violin_state_.timer += 50;
        switch (violin_state_.stage) {
            case 0: if (violin_state_.angle < 90) violin_state_.angle+=9; else if (violin_state_.angle > 90) violin_state_.angle-=9; break;
            case 1: if (violin_state_.angle > 0) violin_state_.angle-=9; break;
            case 2: if (violin_state_.angle < 180) violin_state_.angle+=9; break;
            case 3: if (violin_motor_) violin_motor_->Forward(VIOLIN_SPEED_PERCENT); break;
            case 4: if (violin_state_.angle > 90) violin_state_.angle-=9; else if (violin_state_.angle < 90) violin_state_.angle+=9; break;
            case 5: if (violin_state_.angle < 180) violin_state_.angle+=9; break;
            case 6: if (violin_state_.angle > 0) violin_state_.angle-=9; break;
            case 7: if (violin_motor_) violin_motor_->Reverse(VIOLIN_SPEED_PERCENT); break;
            case 8: if (violin_state_.angle < 130) violin_state_.angle+=9; else if (violin_state_.angle > 130) violin_state_.angle-=9; break;
            case 9: if (violin_state_.angle > 90) violin_state_.angle-=9; else if (violin_state_.angle < 90) violin_state_.angle+=9; break;
        }
        if (violin_state_.angle < 0) violin_state_.angle = 0;
        if (violin_state_.angle > 180) violin_state_.angle = 180;
        if (violin_servo_) violin_servo_->SetAngle(violin_state_.angle);
        int st_dur[] = {500,800,1000,1000,600,1000,1200,1000,1500,300};
        if (violin_state_.timer >= st_dur[violin_state_.stage]) {
            if (violin_state_.stage == 3) { violin_motor_->Stop(); violin_state_.fwd_count++; }
            else if (violin_state_.stage == 7) { violin_motor_->Stop(); violin_state_.rev_count++; }
            violin_state_.stage++;
            if (violin_state_.stage >= 10) { violin_state_.stage = 0; violin_state_.loop_count++; if (violin_state_.loop_count >= 3) violin_state_.loop_count = 0; }
            violin_state_.timer = 0;
        }


        if (dog_servo_) {
                    // 小狗摇尾：随机方�?频率�?~60° 范围
            if (dog_state_.pause > 0) {
                dog_state_.pause--;
            } else {
                dog_state_.angle += dog_state_.dir;
                if (dog_state_.angle >= dog_state_.target) {
                    dog_state_.dir = -1;
                    dog_state_.pause = 20 + (esp_random() % 40);
                }
                if (dog_state_.angle <= 0) {
                    dog_state_.dir = 1;
                    dog_state_.target = 40 + (esp_random() % 21);
                    dog_state_.pause = 0;
                }
                dog_servo_->SetAngle(dog_state_.angle);
            }
        }


        led_toggle++;
                // LED 交替闪烁：每 6 帧（~300ms）切换一�?
        if (led_toggle >= 6) {
            led_toggle = 0;
            led_state = !led_state;
            gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
        }


                // 精确 50ms 帧率控制�?0fps），微秒级定时防累积漂移
        next_frame_us += 50000;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
        }
    }
}

/**
 * @brief 舞蹈收尾动作序列
 *
 * 依次执行�?
 * - LED 全亮
 * - M1 齿轮角度归零（根据正反转时间�?× 转�?~279°/s 补偿�?
 * - 反转补偿 ×1.3 系数（反转电机效率低�?
 * - 小提琴手臂归中（统计正反转次数差 × 补转时间�?
 * - 小狗回退 + 狗尾归位（扫�?180°�?
 * - 大门关闭
 */
void CuckooStateMachine::RunDanceFinale() {

    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);


    if (m1_fwd_time_ > 0 || m1_rev_time_ > 0) {
            // ---- M1 齿轮角度归零：根据正反转时间差（转�?~279°/s）计算需要补偿的角度 ----
        long net = ((long)(m1_fwd_time_ - m1_rev_time_) * 279) / 1000;  // 旋转角度�?正转,-反转
        int rem = (int)(net % 360); if (rem < 0) rem = -rem;  // 取绝对�?
        int ms; bool go_fwd;
        if (rem <= 180) {
            ms = rem * 1000 / 279;
            go_fwd = (net < 0);  // net为负=反转太多，补正转
        } else {
            ms = (360 - rem) * 1000 / 279;
            go_fwd = (net >= 0);  // net为正=正转太多，补反转绕一�?
        }
        // 反转<正转时，补偿×1.3
        if (ms > 0 && m1_fwd_time_ > m1_rev_time_) {
            ms = ms * 135 / 100;
        }
        if (ms > 0) {
            ESP_LOGI(TAG, "Dance: final: net=%lddeg rem=%ddeg, %s %dms", net, rem, go_fwd ? "fwd" : "rev", ms);
            if (go_fwd) m1_->Forward(100); else m1_->Reverse(100);
            vTaskDelay(pdMS_TO_TICKS(ms));
            m1_->Stop();
        }
    }


        // ---- 小提琴手臂平衡：统计正反转次数差，补转使手臂大致归中 ----
    if (violin_state_.rev_count < violin_state_.fwd_count && violin_motor_) {
        ESP_LOGI(TAG, "Violin: fwd=%d rev=%d, adding reverse", violin_state_.fwd_count, violin_state_.rev_count);
        violin_motor_->Reverse(VIOLIN_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(VIOLIN_BALANCE_TIME_MS));
    } else if (violin_state_.fwd_count < violin_state_.rev_count && violin_motor_) {
        ESP_LOGI(TAG, "Violin: fwd=%d rev=%d, adding forward", violin_state_.fwd_count, violin_state_.rev_count);
        violin_motor_->Forward(VIOLIN_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(VIOLIN_BALANCE_TIME_MS));
    }
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);


    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(920));
        m3_->Stop();
    }

    if (dog_servo_) {
            // 小狗尾巴归位：从当前角度扫描�?180°（关门）
        int dog_cur = dog_state_.angle;
        dog_servo_->Sweep(dog_cur, 180, (180 - dog_cur) * 15);
        dog_state_.angle = 180;
    }

    MotorPowerOn();
    if (m2_) {
            // 大门关闭
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
}

// ============================================
// NVS 闹钟持久化存�?
// ============================================

static const char* kAlarmNvsNamespace = "cuckoo_alarm";
static const char* kAlarmNvsKey = "alarms";

/**
 * @brief 将闹钟数组写�?NVS 闪存
 *
 * 携带 alarm_mutex_ 锁防并发写。blob 格式直接写入 alarms_ 数组�?
 */
void CuckooStateMachine::SaveAlarmsToNvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kAlarmNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SaveAlarms: nvs_open failed %d", err);
        return;
    }
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    err = nvs_set_blob(handle, kAlarmNvsKey, alarms_, sizeof(alarms_));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SaveAlarms: nvs_set/commit failed %d", err);
    } else {
        ESP_LOGI(TAG, "Alarms saved to NVS (%d slots)", kMaxAlarms);
    }
}

/**
 * @brief �?NVS 闪存读取闹钟数组并统计有效闹钟数
 *
 * 携带 alarm_mutex_ 锁防并发读。blob 格式直接读取�?alarms_ 数组�?
 */
void CuckooStateMachine::LoadAlarmsFromNvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kAlarmNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "LoadAlarms: no saved alarms (nvs_open %d)", err);
        return;
    }
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    size_t required_size = sizeof(alarms_);
    err = nvs_get_blob(handle, kAlarmNvsKey, alarms_, &required_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "LoadAlarms: nvs_get_blob failed %d", err);
        return;
    }

    int count = 0;
    for (int i = 0; i < kMaxAlarms; i++) {
        if (alarms_[i].enabled) count++;
    }
    alarm_count_ = count;
    ESP_LOGI(TAG, "Alarms loaded from NVS (%d active)", count);
}

// ===== Part 4: 演出/报时/闹钟/狗秀/琳达/花园/音乐舞蹈/粤语查询 (L2395-4412 / 原始行号) =====
// ============================================
// Part 4 包含以下模块�?
// - StartPerformance / PerformanceTask: 整点/半点/手动表演统一入口
// - CheckTime: 时间触发（整�?半点�?
// - 闹钟系统: SetAlarm / GetAlarmsJson / DeleteAlarm / StopAlarm / CheckAlarms / AlarmTask
// - DogShow / DogShowTask: 小狗独立表演
// - LindaShow / GardenShow: 琳达/花园主题表演
// - start_show / ShowTask: 随机歌曲+舞蹈表演
// - MusicDanceTick / MusicDogIntro/Outro: 在线音乐播放时的实时动作
// - SelectMusicUrl / CheckMultiArtist / CantoneseLookup: QQ音乐多版本选歌+粤语
// ============================================
/**
 * @brief 启动表演（整�?半点/手动触发统一入口�?
 *
 * 前置检查：
 * - 已有表演进行�?�?拒绝
 * - AI 唤醒�?5 秒内 �?拒绝（防误触发）
 * - AI 正在说话/监听 �?AbortSpeaking + 200ms 等待进入 idle
 *
 * 成功后禁用唤醒词检�?+ 语音处理，在 Core 1 创建 PerformanceTask�?
 *
 * @param type 表演类型：kPerformanceHour(整点)/kPerformanceHalf(半点)/kPerformanceManual(手动)
 * @param hour 小时�?2小时制，整点需此参数）
 */
void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
        // 防止重入：已有表演在进行�?
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---


        // 检查距上次idle退出是否不�?秒（5000000us�?
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {
            // 如果�?秒窗口内 �?拒绝（AI可能还在处理中）
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }


    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
        // ---- 等待AI完成当前话语/监听，防止表演与TTS冲突 ----
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
            // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断�?---
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
            // 等待200ms确保AI完全转入idle状�?
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    app.GetAudioService().EnableVoiceProcessing(false);

    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
            total_calls_ = hour;
            call_count_ = 3;
            break;
        case kPerformanceHalf:
            total_calls_ = 0;
            call_count_ = 3;
            break;
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
            // 重置运行标志，允许下次表演触�?
        default: is_running_ = false; return;
    }

    xTaskCreatePinnedToCore(
        PerformanceTask,
        "cuckoo_perf",
        4096,
        this,
        5,
        nullptr,
        1
    );
}


/**
 * @brief 表演任务（在Core 1执行）——整�?半点/手动统一入口
 *
 * 整点：报时N�?+ 0013.mp3循环N�?+ 舞蹈(LED+水车+跳舞电机+小提�?小狗)
 * 半点：报�?声，无音乐仅舞蹈
 * 手动：指定唤醒词触发后播放音�?+ 舞蹈
 */
void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Performance task started");

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);


    if (sm->current_performance_ == kPerformanceHour) {

                    // Phase 1: 大门开 + 小鸟开�?+ 报时N�?+ 大门�?
        sm->OpenBirdDoor();
        for (int i = 0; i < sm->call_count_; i++) {

            if (sm->bell_player_) {
                    // 停止布谷鸟声
                sm->bell_player_->PlayCuckooSoundAsync();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            sm->BirdJumpPulse();
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(1800));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
        sm->CloseBirdDoor();


        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            int16_t* bell_pcm = nullptr;
            size_t bell_samples = 0;
            int bell_sr = 0;
            int dec_ret = sm->mp3_->DecodeToBuffer(13, &bell_pcm, &bell_samples, &bell_sr);
            if (dec_ret == 0 && bell_pcm && bell_samples > 0) {
                ESP_LOGI(TAG, "Bell PCM loaded: %u samples @ %d Hz", (unsigned)bell_samples, bell_sr);
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    app.GetAudioService().OutputRawPcm(bell_pcm, bell_samples, bell_sr);

                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                heap_caps_free(bell_pcm);
            } else {

                ESP_LOGW(TAG, "DecodeToBuffer failed, falling back to PlayIndex");
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    sm->mp3_->PlayIndex(13);
                    int wait_start = 0;
                    while (wait_start < 5 && sm->is_running_ && !sm->mp3_->IsPlaying()) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wait_start++;
                    }
                    int wait_done = 0;
                    while (wait_done < 50 && sm->is_running_ && sm->mp3_->IsPlaying()) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wait_done++;
                    }
                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(300));
                    }
                }
            }
        }


    // Phase 2: 音乐+舞蹈（仅�?hourly_perf 开启时执行�?
    if (sm->hourly_perf_.load()) {
        app.GetAudioService().SetOutputMuted(false);
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);

        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            sm->mp3_->SetDisableDucking(true);
            int song = sm->total_calls_;
            sm->show_music_index_ = song;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = (CuckooStateMachine*)arg;
                    // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
                s->PlayShowMusicBg(s->show_music_index_);
                vTaskDelete(nullptr);
            }, "show_bg", 4096, sm, 4, nullptr, 1);
            int wait_start = 0;
            while (wait_start < 6 && sm->is_running_ && !Application::GetInstance().GetAudioService().IsBgAudioActive()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                wait_start++;
            }
            sm->violin_state_.fwd_count = 0;
            sm->violin_state_.rev_count = 0;


            unsigned long intro_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = static_cast<CuckooStateMachine*>(arg);
                    // 舞蹈开场：小狗出来
                s->RunDanceIntro();
                vTaskDelete(nullptr);
            }, "dance_intro", 2048, sm, 5, nullptr, 1);


                // 舞蹈循环：M1电机+小提�?小狗摇尾+LED闪烁（音乐播放期间持续）
            sm->RunDanceLoop();

            // 开场（开�?小狗，异步，~5s）必须在收尾前完�?
            // 否则小狗回退/关门会与开场重�?
            {
                unsigned long since_intro = xTaskGetTickCount() * portTICK_PERIOD_MS - intro_start_ms;
                if (since_intro < 6000) vTaskDelay(pdMS_TO_TICKS(6000 - since_intro));
            }
                // 舞蹈收尾：电机归�?小提琴平�?小狗回退+大门关闭
            sm->RunDanceFinale();
        }

    // 关闭水车 + 灯光熄灭
        if (sm->water_bird_) sm->water_bird_->Stop();
            // 控制LED灯（A和B交替闪烁�?
        gpio_set_level(LED_A_GPIO, 0);
        gpio_set_level(LED_B_GPIO, 0);
    } else {
        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== 半点报时�?声钟�?+ 舞蹈 ======
    } else if (sm->current_performance_ == kPerformanceHalf) {
                    // Phase 1: 大门开 + 小鸟开�?+ 报时N�?+ 大门�?
        sm->OpenBirdDoor();
        for (int i = 0; i < sm->call_count_; i++) {

            if (sm->bell_player_) {
                    // 停止布谷鸟声
                sm->bell_player_->PlayCuckooSoundAsync();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            sm->BirdJumpPulse();
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(1800));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
        sm->CloseBirdDoor();

        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== 表演完成 ======
        // 停止MP3播放
    if (sm->mp3_) sm->mp3_->Stop();


    sm->current_phase_ = kPhaseIdle;
        // 表演结束，电机断�?
    sm->MotorPowerOff();


    if (app.GetDeviceState() == kDeviceStateIdle) {
            // 表演结束后恢复唤醒词检测（阈�?.02=高灵敏度�?
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
/**
 * @brief 时间触发检查（�?250ms tick 调用一次）
 *
 * 检查是否满足报时条件（整点/半点），以及静音模式/设备忙等跳过条件�?
 * 4 档静音模式：0=始终静音, 1=始终允许, 2=光线暗时静音, 3=时间段静�?
 *
 * @param hour  当前小时
 * @param min   当前分钟
 * @param dark  环境光线状态（true=暗）
 */
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    is_dark_ = dark;  // 更新环境光线状�?
    // ---- 前置条件检查：表演�?/ 设备�?/ 静音模式 ----
    if (is_running_) {
        ESP_LOGI(TAG, "Skipping chime: performance already running");
        return;
    }

    // 设备忙（非idle状态或bg audio活跃）→ 跳过报时
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle || Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        ESP_LOGI(TAG, "Skipping chime: device busy (state=%d, bg_audio=%d)", state,
                 (int)Application::GetInstance().GetAudioService().IsBgAudioActive());
        return;
    }

    // ---- 静音模式判断�?档）----
    int mode = quiet_mode_.load();
    if (mode == 0) {
        // 模式0: 始终静音
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=0 (always silent)");
        return;
    }
    if (mode == 2 && is_dark_) {
        // 模式2: 光线传感器检测黑暗时静音（不限时间）
        int ldr_raw = ldr_ ? ldr_->ReadRaw() : -1;
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=2 (dark-silent) LDR=%d threshold=%d", ldr_raw, LDR_DARK);
        return;
    }
    if (mode == 3) {
        // 模式3: 时间段静音（start~end区间内静音，支持跨夜�?
        int start = quiet_start_.load();
        int end = quiet_end_.load();
        if (start < end) {
            if (hour >= start && hour < end) {
                ESP_LOGI(TAG, "Skipping chime: quiet_mode=3 window %d-%d", start, end);
                return;
            }
        } else {
            // 跨夜区间（如22:00~6:00）：hour>=start �?hour<end 都在区间�?
            if (hour >= start || hour < end) {
                ESP_LOGI(TAG, "Skipping chime: quiet_mode=3 overnight window %d-%d", start, end);
                return;
            }
        }
    }





    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting", hour, min);
        StartPerformance(kPerformanceHour, hour);
        MarkHourlyChime(hour);  // 触发成功后去重，防止重复整点报时
    }

    else if (min == 30) {
        ESP_LOGI(TAG, "Half-hour chime: %d:%02d, starting mini performance", hour, min);
            // ====== 半点报时�?声钟�?+ 舞蹈 ======
        StartPerformance(kPerformanceHalf, hour);
        MarkHalfHourlyChime(hour);  // 触发成功后去重，防止重复半点报时
    }
}

/**
 * @brief 设置当前时间
 *
 * @param hour 小时
 * @param min 分钟
 * @param sec �?
 */
void CuckooStateMachine::SetTime(int hour, int min, int sec) {
        // 设置全局时间变量
    current_hour_ = hour % 24;
    current_min_ = min % 60;
    current_sec_ = sec % 60;
        // 标记时间已设置，允许报时/闹钟触发
    time_set_ = true;
    ESP_LOGI(TAG, "Time set to %02d:%02d:%02d", current_hour_.load(), current_min_.load(), current_sec_.load());
}

/**
 * @brief 获取当前时间
 *
 * @param hour [out] 小时
 * @param min [out] 分钟
 */
void CuckooStateMachine::GetTime(int &hour, int &min) {
    hour = current_hour_;
    min = current_min_;
}

// ============================================

// ============================================
/**
 * @brief 保存静音模式配置�?NVS
 */
void CuckooStateMachine::SaveQuietMode() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i8(nvs, "qmode", (int8_t)quiet_mode_.load());
        nvs_set_i8(nvs, "qstart", (int8_t)quiet_start_.load());
        nvs_set_i8(nvs, "qend", (int8_t)quiet_end_.load());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/**
 * @brief �?NVS 加载静音模式配置
 */
void CuckooStateMachine::LoadQuietMode() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "qmode", &val) == ESP_OK) quiet_mode_ = (int)val;
        if (nvs_get_i8(nvs, "qstart", &val) == ESP_OK) quiet_start_ = (int)val;
        if (nvs_get_i8(nvs, "qend", &val) == ESP_OK) quiet_end_ = (int)val;
        nvs_close(nvs);
    }
}

/**
 * @brief 保存 Kids 模式状态到 NVS
 */
void CuckooStateMachine::SaveKidsActive() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i8(nvs, "kids", (int8_t)kids_active_.load());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/**
 * @brief �?NVS 加载 Kids 模式状�?
 */
void CuckooStateMachine::LoadKidsActive() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "kids", &val) == ESP_OK) kids_active_ = (bool)val;
        nvs_close(nvs);
    }
}

/**
 * @brief 保存整点表演开关到 NVS
 */
void CuckooStateMachine::SaveHourlyPerf() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i8(nvs, "hperf", (int8_t)hourly_perf_.load());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/**
 * @brief �?NVS 加载整点表演开�?
 */
void CuckooStateMachine::LoadHourlyPerf() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "hperf", &val) == ESP_OK) hourly_perf_ = (bool)val;
        nvs_close(nvs);
    }
}

// ============================================

// ============================================
/**
 * @brief 设置闹钟
 *
 * 若已存在相同时间点的闹钟则更�?enabled/repeat_daily 字段；否则追加新闹钟�?
 * 操作后立即调�?SaveAlarmsToNvs 持久化�?
 *
 * @param hour         小时 (0-23)
 * @param minute       分钟 (0-59)
 * @param repeat_daily 是否每天重复
 */
void CuckooStateMachine::SetAlarm(int hour, int minute, bool repeat_daily) {
        // 加锁保护闹钟数组的并发访�?
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (alarm_count_ >= kMaxAlarms) {
        ESP_LOGW(TAG, "Alarm list full (max %d)", kMaxAlarms);
        return;
    }
 // 检查重复闹�?- 更新已有闹钟
    for (int i = 0; i < alarm_count_; i++) {
            // 小时和分钟都匹配且到达整�?�?触发
        if (alarms_[i].hour == hour && alarms_[i].minute == minute) {
            alarms_[i].enabled = true;
            alarms_[i].repeat_daily = repeat_daily;
            ESP_LOGI(TAG, "Alarm updated: %02d:%02d (repeat=%d)", hour, minute, repeat_daily);
                // 修改后立即持久化到NVS
            SaveAlarmsToNvs();
            return;
        }
    }
    alarms_[alarm_count_].hour = hour;
    alarms_[alarm_count_].minute = minute;
    alarms_[alarm_count_].enabled = true;
    alarms_[alarm_count_].repeat_daily = repeat_daily;
    alarm_count_++;
    ESP_LOGI(TAG, "Alarm set: %02d:%02d (repeat=%d, total=%d)",
             hour, minute, repeat_daily, alarm_count_.load());
        // 修改后立即持久化到NVS
    SaveAlarmsToNvs();
}

/**
 * @brief 获取闹钟列表 JSON 字符�?
 * @return JSON 数组，每项含 index/hour/minute/enabled/repeat_daily
 */
std::string CuckooStateMachine::GetAlarmsJson() {
        // 加锁保护闹钟数组的并发访�?
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    std::string json = "[";
    for (int i = 0; i < alarm_count_; i++) {
        if (i > 0) json += ", ";
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"index\": %d, \"hour\": %d, \"minute\": %d, \"enabled\": %s, \"repeat_daily\": %s}",
            i + 1, alarms_[i].hour, alarms_[i].minute,
            alarms_[i].enabled ? "true" : "false",
            alarms_[i].repeat_daily ? "true" : "false");
        json += buf;
    }
    json += "]";
    return json;
}

/**
 * @brief 删除指定闹钟
 *
 * @param index 闹钟索引�?=第一个）
 */
bool CuckooStateMachine::DeleteAlarm(int index) {
        // 加锁保护闹钟数组的并发访�?
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (index < 1 || index > alarm_count_) {
        ESP_LOGW(TAG, "Invalid alarm index: %d (have %d alarms)", index, alarm_count_.load());
        return false;
    }
 // 剩余闹钟顺序下移
    for (int i = index - 1; i < alarm_count_ - 1; i++) {
        alarms_[i] = alarms_[i + 1];
    }
    alarm_count_--;
    ESP_LOGI(TAG, "Alarm %d deleted (remaining: %d)", index, alarm_count_.load());
        // 修改后立即持久化到NVS
    SaveAlarmsToNvs();
    return true;
}

/**
 * @brief 停止当前闹钟响铃
 *
 * 设置 alarm_stopped_ 标志位，通知 AlarmTask 退出循环�?
 * 同时将一次性闹钟标记为禁用（重复闹钟不受影响）�?
 */
void CuckooStateMachine::StopAlarm() {
    alarm_stopped_ = true;
    alarm_ringing_ = false;
 // 一次性闹钟，用户停止后禁�?
    {
            // 加锁保护闹钟数组的并发访�?
        std::lock_guard<std::mutex> lock(alarm_mutex_);
        for (int i = 0; i < alarm_count_; i++) {
                // 一次性闹钟：触发后立即禁�?
            if (alarms_[i].enabled && !alarms_[i].repeat_daily
                && alarms_[i].hour == current_hour_
                && alarms_[i].minute == current_min_) {
                alarms_[i].enabled = false;
                ESP_LOGI(TAG, "One-shot alarm %02d:%02d disabled after stop",
                         alarms_[i].hour, alarms_[i].minute);
                SaveAlarmsToNvs();  // 持久化，防止停电后复�?
            }
        }
    }
 // 停止所有当前播放的音频
        // 停止MP3播放
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

/**
 * @brief 检查闹钟触发条�?
 *
 * 每秒 tick 调用。遍历闹钟数组，小时+分钟+�?0) 匹配即触�?AlarmTask�?
 * 同一时间只触发一个闹钟（第一个匹配的）�?
 *
 * @param hour   当前小时
 * @param minute 当前分钟
 * @param sec    当前�?
 */
void CuckooStateMachine::CheckAlarms(int hour, int minute, int sec) {
    if (alarm_ringing_) return;  // 已有闹钟在响，跳�?
    if (!time_set_) return;      // 时间尚未设置，无法触发闹�?

    {
            // 加锁保护闹钟数组的并发访�?
        std::lock_guard<std::mutex> lock(alarm_mutex_);
        for (int i = 0; i < alarm_count_; i++) {
                // 找到第一个未启用的闹钟槽�?
            if (!alarms_[i].enabled) continue;
                // 小时和分钟都匹配且到达整�?�?触发
            if (alarms_[i].hour == hour && alarms_[i].minute == minute && sec == 0) {
                ESP_LOGI(TAG, "Alarm triggered! %02d:%02d", hour, minute);
                alarm_ringing_ = true;
                alarm_stopped_ = false;
                xTaskCreate(
                    AlarmTask,
                    "cuckoo_alarm",
                    4096,
                    this,
                    3,
                    nullptr
                );
                break;  // 同一时间只触发一个闹�?
            }
        }
    }
}

/**
 * @brief 闹钟响铃任务（独�?FreeRTOS task�?
 *
 * 循环播放闹铃：前 10 �?15%�?00% 渐强�?0s），随后全音量重�?50 次（100s）�?
 * 之后进入 2 分钟贪睡期（每秒检�?alarm_stopped_），然后从头循环�?
 */
void CuckooStateMachine::AlarmTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Alarm task started");

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    while (sm->alarm_ringing_ && !sm->alarm_stopped_) {
 // 播放闹铃循环50次（2s/�?= 总计100s�?
 // 第一轮：�?0次音量从15%渐升�?00%�?0s内）
        for (int i = 0; i < 50; i++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            if (sm->mp3_) {
                float volume;
                if (i < 10) {
                    volume = 0.15f + 0.85f * (float)i / 9.0f;
                } else {
                    volume = 1.0f;
                }
                sm->mp3_->PlayAlarmRing(volume);
            }
        }

        if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;

 // 贪睡：等�?分钟，每秒检�?stopped_
        ESP_LOGI(TAG, "Alarm snoozing for 2 minutes...");
        for (int s = 0; s < 120; s++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 清理资源
    app.GetAudioService().SetOutputMuted(false);
    sm->alarm_ringing_ = false;
    sm->alarm_stopped_ = false;
    ESP_LOGI(TAG, "Alarm task ended");
    vTaskDelete(NULL);
}

// ============================================




// ============================================



/**
 * @brief 小狗秀：小狗出门→叫一声→摇尾→叫一声→回退→关�?
 * 狗叫叠加混音到背景音乐上（不打断），若无音乐则OutputRawPcm直出
 */
void CuckooStateMachine::DogShow() {
    if (is_running_) return;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);
    }, "dog_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief DogShow 执行任务（在 DogShow spawn �?task 中执行）
 *
 * 完整狗秀流程：水车启�?�?开�?�?狗尾扫出 �?前进 �?叫一�?�?摇尾 10s
 *            �?再叫一�?�?回退 �?狗尾归位 �?关门 �?断电
 */
void CuckooStateMachine::DogShowTask() {
    auto& app = Application::GetInstance();





    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");
    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");

    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---

    // 步骤1: 启动水车
        // 水车旋转（连续水流效果）
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 步骤2: 打开大门
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
            // 延时等待门动作完�?
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }


    if (dog_servo_) {
                    // 异步推门+叫：狗尾巴扫�?+ 前进 + 叫一�?+ 复位
            // 小狗尾巴扫出
        dog_servo_->Sweep(180, 20, 1200);
        dog_state_.angle = 20;
    }


    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }


    PlayDogBarkDirect();


    if (dog_servo_) {
            // 小狗尾巴微调（准�?归位�?
        dog_servo_->Sweep(20, 0, 300);
        dog_state_.angle = 0;
    }


    unsigned long start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long end_time = start + 10000;
    int sweep_dir = 0;
    while (is_running_ && (xTaskGetTickCount() * portTICK_PERIOD_MS) < end_time) {
            // 水车旋转（连续水流效果）
        if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);
        if (dog_servo_) {
            if (sweep_dir == 0) {
                dog_servo_->Sweep(0, 60, 900);
                dog_state_.angle = 60;
                vTaskDelay(pdMS_TO_TICKS(500));
                sweep_dir = 1;
            } else {
                dog_servo_->Sweep(60, 0, 900);
                dog_state_.angle = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
                sweep_dir = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }


    PlayDogBarkDirect();


    if (dog_servo_) {
            // 小狗尾巴微调（准�?归位�?
        dog_servo_->Sweep(0, 20, 300);
        dog_state_.angle = 20;
    }


    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }


    if (dog_servo_) {
            // 小狗尾巴归位关门
        dog_servo_->Sweep(20, 180, 1200);
        dog_state_.angle = 180;
    }


    if (water_bird_) water_bird_->Stop();


    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
            // 延时等待门动作完�?
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 清理资源
        // 表演结束，电机断�?
    MotorPowerOff();
        // 重置运行标志，允许下次表演触�?
    is_running_ = false;
    current_performance_ = kPerformanceNone;

            // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断�?---
    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈�?.02=高灵敏度�?
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "DogShow: done");
}

/**
 * @brief 通用 WAV 资产播放�?
 *
 * �?assets 分区读取 WAV 文件，解�?WAV 头（仅支�?mono s16），
 * 跳过 RIFF chunk 结构找到 data 块后直接 OutputRawPcm 输出�?
 * dog_bark.wav 特殊处理：无增益直通�?
 *
 * @param filename WAV 文件�?
 */
void CuckooStateMachine::PlayWavAsset(const char* filename) {
    void* wav_ptr = nullptr;
    size_t wav_size = 0;
    if (!Assets::GetInstance().GetAssetData(filename, wav_ptr, wav_size)) {
        ESP_LOGW(TAG, "PlayWavAsset: %s not found", filename);
        return;
    }
        // WAV文件头至�?4字节
    if (wav_size < 44) return;

    const uint8_t* wav_data = (const uint8_t*)wav_ptr;
    uint16_t channels = wav_data[22] | (wav_data[23] << 8);
    uint32_t sample_rate = wav_data[24] | (wav_data[25] << 8) | (wav_data[26] << 16) | (wav_data[27] << 24);
    uint16_t bits = wav_data[34] | (wav_data[35] << 8);
    if (bits != 16 || channels != 1) {
        ESP_LOGW(TAG, "PlayWavAsset: %s need mono s16", filename);
        return;
    }


    const int16_t* pcm = nullptr;
    size_t num_samples = 0;
    for (size_t i = 12; i + 8 < wav_size; ) {
        char tag[5] = {0};
        memcpy(tag, wav_data + i, 4);
        uint32_t chunk_size = wav_data[i+4] | (wav_data[i+5] << 8) | (wav_data[i+6] << 16) | (wav_data[i+7] << 24);
        if (strcmp(tag, "data") == 0) {
            pcm = (const int16_t*)(wav_data + i + 8);
            num_samples = chunk_size / sizeof(int16_t);
            break;
        }
        i += 8 + chunk_size;
        if (chunk_size % 2) i++;
    }
    if (pcm == nullptr || num_samples == 0) {
        ESP_LOGW(TAG, "PlayWavAsset: %s no data chunk", filename);
        return;
    }

    ESP_LOGI(TAG, "PlayWavAsset: %s %u samples, %d Hz", filename, (unsigned)num_samples, sample_rate);
    auto& app = Application::GetInstance();
    bool is_bark = (strcmp(filename, "dog_bark.wav") == 0);
    if (is_bark) {

        app.GetAudioService().OutputRawPcm(pcm, num_samples, sample_rate);
    } else {
        std::vector<int16_t> amplified(num_samples);
        const int gain = 1;
        for (size_t i = 0; i < num_samples; i++) {
            int32_t v = (int32_t)pcm[i] * gain;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            amplified[i] = (int16_t)v;
        }
        app.GetAudioService().OutputRawPcm(amplified.data(), num_samples, sample_rate);
    }
    int play_ms = (int)(num_samples * 1000 / sample_rate);
    vTaskDelay(pdMS_TO_TICKS(play_ms + 100));
}


/**
 * @brief 直接播放狗叫 WAV（不经过混音�?
 */
void CuckooStateMachine::PlayDogBarkDirect() {
    PlayWavAsset("dog_bark.wav");
}

    // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
/**
 * @brief 异步播放背景音乐（走bg audio环形缓冲，不阻塞�?
 *
 * @param index MP3文件编号
 */
bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;


    int16_t* pcm = nullptr;
    size_t total_samples = 0;
    int src_sr = 0;
    if (mp3_->DecodeToBuffer(index, &pcm, &total_samples, &src_sr) < 0) {
            // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
        ESP_LOGE(TAG, "PlayShowMusicBg: decode failed for %04d.mp3", index);
        return false;
    }

        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 %u samples %dHz", index, (unsigned)total_samples, src_sr);

    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetBackgroundAudioGain(1.0f);

    const int DST_SR = 16000;
    const size_t CHUNK_SRC = 2048;  // 分小块推�?
    size_t offset = 0;

    while (offset < total_samples && is_running_) {
        size_t chunk = (total_samples - offset > CHUNK_SRC) ? CHUNK_SRC : (total_samples - offset);


        if (src_sr != DST_SR) {
            float ratio = (float)src_sr / DST_SR;
            std::vector<int16_t> dst(chunk * 2 / 3 + 2);  // 重采样后体积缩小�?0%
            size_t d = 0;
            float pos = 0;
            while (pos < (float)chunk - 1.0f && d < dst.size() - 1) {
                size_t idx = (size_t)pos;
                float frac = pos - idx;
                dst[d++] = (int16_t)((float)pcm[offset + idx] * (1.0f - frac) + (float)pcm[offset + idx + 1] * frac);
                pos += ratio;
            }
            audio.PushBackgroundAudio(dst.data(), d, DST_SR);
        } else {
            audio.PushBackgroundAudio(pcm + offset, chunk, DST_SR);
        }

        audio.EnableBgAudioDrain(true);
        offset += chunk;


        while (audio.GetBgAudioFillLevel() > 64000 && is_running_)
            vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(pcm);


    {
        int drain_wait = 0;
        while (is_running_ && audio.GetBgAudioFillLevel() > 0 && drain_wait < 200) {
            vTaskDelay(pdMS_TO_TICKS(50));
            drain_wait++;
        }
            // 等待200ms确保AI完全转入idle状�?
        vTaskDelay(pdMS_TO_TICKS(200));
            // 清空残留背景音频，防止前一次Opus/PCM残留干扰
        audio.ClearBackgroundAudio();
            // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
        ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 finished, bg audio cleared", index);
    }
    return true;
}

/**
 * @brief 启动琳达秀（spawn LindaShow task�?
 */
void CuckooStateMachine::StartLindaShow() {

    if (is_running_) return;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->LindaShow();
        vTaskDelete(nullptr);
    }, "linda_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief 启动花园秀（spawn GardenShow task�?
 */
void CuckooStateMachine::StartGardenShow() {

    if (is_running_) return;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->GardenShow();
        vTaskDelete(nullptr);
    }, "garden_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief 琳达秀：播�?015.mp3 + 舞蹈开�?循环/收尾
 */
void CuckooStateMachine::LindaShow() {
    if (is_running_) { ESP_LOGW(TAG, "LindaShow: already running, skip"); return; }

    auto& app = Application::GetInstance();



    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");


        // 控制LED灯（A和B交替闪烁�?
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));


        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
    if (mp3_) { mp3_->SetDisableDucking(true); int song = LINDA_SONG_BASE + (linda_song_counter_++ % LINDA_SONG_COUNT); ESP_LOGI(TAG, "LindaShow: playing %04d.mp3 (counter=%d)", song, (int)linda_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }


    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }




    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long m1_timer = music_start_ms;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;
    m1_fwd_time_ = 0;
    m1_rev_time_ = 0;
    m1_stage_start_ = music_start_ms;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s 安全超时ty timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;


        if (!led_final && elapsed >= 24000) {
                // 控制LED灯（A和B交替闪烁�?
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }


        switch (m1_stage) {
            case 0:

                if (m1_) m1_->Forward(DANCE_SPEED_PERCENT);

                if (now - m1_timer > m1_rand_time) {
                    m1_fwd_time_ += (now - m1_stage_start_);
                    m1_stage_start_ = now;
                    m1_timer = now; m1_stage = 1;
                }

                break;
            case 1:
                    // 停止所有电�?
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 2;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
            case 2:

                if (m1_) m1_->Reverse(DANCE_SPEED_PERCENT);

                if (now - m1_timer > m1_rand_time) {
                    m1_rev_time_ += (now - m1_stage_start_);
                    m1_stage_start_ = now;
                    m1_timer = now; m1_stage = 3;
                }

                break;
            case 3:
                    // 停止所有电�?
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }

        if (!led_final) {
            led_toggle++;
            if (led_toggle >= 6) {
                led_toggle = 0;
                led_state = !led_state;
                    // 控制LED灯（A和B交替闪烁�?
                gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
                gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }


        // 停止所有电�?
    if (m1_) m1_->Stop();


    if (m1_fwd_time_ > 0 || m1_rev_time_ > 0) {
        long net = ((long)(m1_fwd_time_ - m1_rev_time_) * 279) / 1000;
        int rem = (int)(net % 360); if (rem < 0) rem = -rem;
        int ms; bool go_fwd;
        if (rem <= 180) {
            ms = rem * 1000 / 279;
            go_fwd = (net < 0);
        } else {
            ms = (360 - rem) * 1000 / 279;
            go_fwd = (net >= 0);
        }
        // 反转<正转时，补偿×1.3
        if (ms > 0 && m1_fwd_time_ > m1_rev_time_) {
            ms = ms * 135 / 100;
        }
        if (ms > 0) {
            ESP_LOGI(TAG, "LindaShow: final: net=%lddeg rem=%ddeg, %s %dms", net, rem, go_fwd ? "fwd" : "rev", ms);
            if (go_fwd) m1_->Forward(100); else m1_->Reverse(100);
            vTaskDelay(pdMS_TO_TICKS(ms));
                // 停止所有电�?
            m1_->Stop();
        }
    }


    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "LindaShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

        // 控制LED灯（A和B交替闪烁�?
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

        // 停止MP3播放
    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
        // 表演结束，电机断�?
    MotorPowerOff();
        // 重置运行标志，允许下次表演触�?
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);
    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈�?.02=高灵敏度�?
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "LindaShow: done");
}

/**
 * @brief 花园秀：播�?016.mp3 + 舞蹈
 */
void CuckooStateMachine::GardenShow() {
    if (is_running_) { ESP_LOGW(TAG, "GardenShow: already running, skip"); return; }

    auto& app = Application::GetInstance();



    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");


        // 控制LED灯（A和B交替闪烁�?
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));


        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
    if (mp3_) { mp3_->SetDisableDucking(true); int song = GARDEN_SONG_BASE + (garden_song_counter_++ % GARDEN_SONG_COUNT); ESP_LOGI(TAG, "GardenShow: playing %04d.mp3 (counter=%d)", song, (int)garden_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }


    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }


    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int violin_angle = SERVO_CENTER_ANGLE;
    int violin_dir = 0;  // 0=小提琴角度递减, 1=递增
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s 安全超时ty timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        if (!led_final && elapsed >= 24000) {
                // 控制LED灯（A和B交替闪烁�?
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }


        if (violin_servo_) {
            if (violin_dir == 0) {
                violin_angle -= 6;
                if (violin_angle <= 0) { violin_angle = 0; violin_dir = 1; }
            } else {
                violin_angle += 6;
                if (violin_angle >= 140) { violin_angle = 140; violin_dir = 0; }
            }
            violin_servo_->SetAngle(violin_angle);
        }
        if (!led_final) {
            led_toggle++;
            if (led_toggle >= 6) {
                led_toggle = 0;
                led_state = !led_state;
                    // 控制LED灯（A和B交替闪烁�?
                gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
                gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }


    if (violin_servo_) {
        violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000);
    }


    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "GardenShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

        // 控制LED灯（A和B交替闪烁�?
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

        // 停止MP3播放
    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
        // 表演结束，电机断�?
    MotorPowerOff();
        // 重置运行标志，允许下次表演触�?
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);
    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈�?.02=高灵敏度�?
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "GardenShow: done");
}

/**
 * @brief 舞蹈全套：开场→循环→收尾→断电
 */
void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Dance start (Arduino-style)");


    unsigned long start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long end = start + 8000;

    int m1_stage = 0;
    unsigned long m1_timer = start;
    int m1_rand_time = 1000 + (esp_random() % 2001);

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS) < end) {
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        switch (m1_stage) {
            case 0:
                if (m1_) m1_->Forward(DANCE_SPEED_PERCENT);
                if (now - m1_timer > m1_rand_time) {
                    m1_timer = now;
                    m1_stage = 1;
                }
                break;
            case 1:
                    // 停止所有电�?
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now;
                    m1_stage = 2;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
            case 2:
                if (m1_) m1_->Reverse(DANCE_SPEED_PERCENT);
                if (now - m1_timer > m1_rand_time) {
                    m1_timer = now;
                    m1_stage = 3;
                }
                break;
            case 3:
                    // 停止所有电�?
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now;
                    m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }


        if (violin_motor_) violin_motor_->Forward(VIOLIN_WARMUP_PERCENT);


        if (violin_servo_) violin_servo_->Sweep(45, 135, 1500);

        vTaskDelay(pdMS_TO_TICKS(50));
    }

        // 停止所有电�?
    if (m1_) m1_->Stop();
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
        // 重置运行标志，允许下次表演触�?
    is_running_ = false;
        // 表演结束，电机断�?
    MotorPowerOff();
    ESP_LOGI(TAG, "Dance finished");
}

// ============================================


// ============================================
/**
 * @brief 电机测试：所有电机顺序运行指定秒�?
 *
 * @param seconds 运行秒数
 */
void CuckooStateMachine::MotorTest(int seconds) {
    if (is_running_) return;
    is_running_ = true;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    ESP_LOGI(TAG, "MotorTest: M1 forward %d seconds...", seconds);
    if (m1_) m1_->Forward(100);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
        // 停止所有电�?
    if (m1_) m1_->Stop();
        // 表演结束，电机断�?
    MotorPowerOff();
        // 重置运行标志，允许下次表演触�?
    is_running_ = false;
    ESP_LOGI(TAG, "MotorTest: done");
}


/**
 * @brief 启动背景音乐+舞蹈表演
 *
 * 随机选取 1-12 首歌，通过 PlayShowMusicBg �?bg audio 环形缓冲播放�?
 * 同时执行 RunDanceIntro→Loop→Finale 完整舞蹈序列�?
 */
void CuckooStateMachine::StartShow() {

    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }

    if (is_running_) {
        StopAll();
            // 等待200ms确保AI完全转入idle状�?
        vTaskDelay(pdMS_TO_TICKS(200));
    }


    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;





    xTaskCreatePinnedToCore(
        [](void* arg) {
            auto* sm = static_cast<CuckooStateMachine*>(arg);
            sm->StartShowTask();
            vTaskDelete(nullptr);
        },
        "cuckoo_show",
        4096,
        this,
        5,
        nullptr,
        1
    );
}

/**
 * @brief StartShow 的入�?task（设置唤醒阈�?�?MotorPowerOn �?调用 ShowTask�?
 */
void CuckooStateMachine::StartShowTask() {
    auto& app0 = Application::GetInstance();
        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?


    ESP_LOGI(TAG, "Show: starting immediately (bg audio + ducking handles overlap)");

    app0.GetAudioService().SetWakeWordThreshold(0.99f);

MotorPowerOn();
    // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

    ShowTask(this);
}

/**
 * @brief 演出主任务（Core 1�?
 *
 * LED+水车启动 �?随机选歌 �?PlayShowMusicBg 后台播放 �?等待 bg audio 就绪
 * �?RunDanceIntro(异步) �?RunDanceLoop(同步) �?音乐结束 �?唤醒词阈值恢�?
 * �?RunDanceFinale �?水车/LED 关闭 �?断电
 */
void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Show task started");

    auto& app = Application::GetInstance();


    if (sm->mp3_) sm->mp3_->SetDisableDucking(true);


    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
            // 水车旋转（连续水流效果）
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);


    int next = (esp_random() % 12) + 1;
    if (next == sm->show_music_index_ && next < 12) {
        next++;
    } else if (next == sm->show_music_index_) {
        next = 1;
    }
    sm->show_music_index_ = next;
        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?


    if (sm->mp3_) {
        sm->mp3_->SetDisableDucking(true);
        xTaskCreatePinnedToCore([](void* arg) {
            auto* s = (CuckooStateMachine*)arg;
                // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲�?
            s->PlayShowMusicBg(s->show_music_index_);
            vTaskDelete(nullptr);
        }, "show_bg", 4096, sm, 4, nullptr, 1);
    }

    // 等待音乐开始播放（bg audio 缓冲就绪�?
    int wait_start = 0;
    while (wait_start < 6 && sm->is_running_ && !Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_start++;
    }

    sm->violin_state_.fwd_count = 0;
    sm->violin_state_.rev_count = 0;
    ESP_LOGI(TAG, "DOG-MOTOR: m3_=%p", sm->m3_);


    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
            // 舞蹈开场：小狗出来
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);


        // 舞蹈循环：M1电机+小提�?小狗摇尾+LED闪烁（音乐播放期间持续）
    sm->RunDanceLoop();






    if (sm->mp3_) sm->mp3_->SetDisableDucking(false);


    if (app.GetDeviceState() != kDeviceStateIdle) {
            // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断�?---
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    app.GetAudioService().EnableBgAudioDrain(false);
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "Show: music ended, wake word threshold will be restored by clock task");

        // 舞蹈收尾：电机归�?小提琴平�?小狗回退+大门关闭
    sm->RunDanceFinale();
        // 重置运行标志，允许下次表演触�?
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;


    if (sm->water_bird_) sm->water_bird_->Stop();
        // 控制LED灯（A和B交替闪烁�?
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

        // 表演结束，电机断�?
    sm->MotorPowerOff();




    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈�?.02=高灵敏度�?
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    else
        app.GetAudioService().SetWakeWordThreshold(0.30f);

    ESP_LOGI(TAG, "Show task finished");
    vTaskDelete(NULL);
}

/**
 * @brief 播放指定编号�?MP3 音乐
 *
 * @param index MP3文件编号
 */
void CuckooStateMachine::PlayMusic(int index) {
    if (mp3_) mp3_->PlayBgMusic(index);
}

/**
 * @brief 紧急停止所有电�?音频
 *
 * 设置 is_running_=false，停止全�?6 路电机，舵机归位 90°，LED 全灭�?
 * �?bg audio 活跃则保留（不打断音乐），否�?stop mp3�?
 */
void CuckooStateMachine::StopAll() {
        // 重置运行标志，允许下次表演触�?
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
        // 停止所有电�?
    if (m1_) m1_->Stop();
    if (m2_) m2_->Stop();
    if (m3_) m3_->Stop();
    if (m4_) m4_->Stop();
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
        // 控制LED灯（A和B交替闪烁�?
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
                // 停止MP3播放
            mp3_->Stop();
        }
    }


    auto state = Application::GetInstance().GetDeviceState();
        // ---- 等待AI完成当前话语/监听，防止表演与TTS冲突 ----
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        Application::GetInstance().Schedule([]() {
            auto s = Application::GetInstance().GetDeviceState();
                // ---- 等待AI完成当前话语/监听，防止表演与TTS冲突 ----
            if (s == kDeviceStateSpeaking || s == kDeviceStateListening) {
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            }
            Application::GetInstance().GetAudioService().EnableVoiceProcessing(true);
                // 表演结束后恢复唤醒词检测（阈�?.02=高灵敏度�?
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
            Application::GetInstance().GetAudioService().RefreshOutputTimestamp();
            Application::GetInstance().GetAudioService().RefreshInputTimestamp();
        });
    }
    ESP_LOGI(TAG, "All motors stopped");
        // 表演结束，电机断�?
    MotorPowerOff();
}

/**
 * @brief 停止背景音乐播放并清空环形缓�?
 */
void CuckooStateMachine::StopMusic() {
    if (mp3_) {
            // 停止MP3播放
        mp3_->Stop();
            // 清空残留背景音频，防止前一次Opus/PCM残留干扰
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

/**
 * @brief 大门正向开门（M2 Forward �?delay �?Stop �?断电�?
 */
void CuckooStateMachine::OpenDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
            // 延时等待门动作完�?
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
        // 表演结束，电机断�?
    MotorPowerOff();
}

/**
 * @brief 大门反向关门（M2 Reverse �?delay �?Stop �?断电�?
 */
void CuckooStateMachine::CloseDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
            // 延时等待门动作完�?
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
        // 表演结束，电机断�?
    MotorPowerOff();
}

/**
 * @brief 鸟跳一次（GPIO 直驱 IN2�?00ms 脉冲 + 400ms 冷却�?
 */
void CuckooStateMachine::BirdJumpOnce() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
        // 表演结束，电机断�?
    MotorPowerOff();
}

/**
 * @brief 鸟跳脉冲（停止水车→GPIO 直驱 IN2 500ms→不等待�?
 */
void CuckooStateMachine::BirdJumpPulse() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
}

/**
 * @brief 鸟跳短脉冲（随机 50~200ms + 300~700ms 冷却�?
 */
void CuckooStateMachine::BirdJumpShort() {



    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    int pulse = 50 + (esp_random() % 151);
    vTaskDelay(pdMS_TO_TICKS(pulse));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    int cooldown = 300 + (esp_random() % 401);
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

/**
 * @brief 音乐舞蹈 tick�?50ms 周期）：在线音乐播放期间的小�?小提琴动作控�?
 */
void CuckooStateMachine::MusicDanceTick() {



    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();

    static bool was_kids_active = true;
        // 检查Kids模式是否激�?
    bool kids_now = kids_active_.load();

    if (music_playing && !IsRunning()) {
        if (music_dance_enabled_ != 1) {
            music_dance_phase_ = 0;
            music_dance_enabled_ = 1;

            dog_outro_done_ = false;
                // 检查Kids模式是否激�?
            if (kids_now) {
                xTaskCreatePinnedToCore([](void* arg) {
                        // 触发小狗出场动作
                    ((CuckooStateMachine*)arg)->MusicDogIntro();
                    vTaskDelete(nullptr);
                }, "dog_intro", 4096, this, 5, nullptr, 1);
            }
                // 检查Kids模式是否激�?
            was_kids_active = kids_now;
        }


            // 检查Kids模式是否激�?
        if (!was_kids_active && kids_now) {
            was_kids_active = true;
            dog_outro_done_ = false;
            xTaskCreatePinnedToCore([](void* arg) {
                    // 触发小狗出场动作
                ((CuckooStateMachine*)arg)->MusicDogIntro();
                vTaskDelete(nullptr);
            }, "dog_intro", 4096, this, 5, nullptr, 1);
        }

            // 检查Kids模式是否激�?
        if (was_kids_active && !kids_now) {
            was_kids_active = false;
                // 停止所有电�?
            if (m1_) m1_->Stop();
            if (violin_servo_) violin_servo_->SetAngle(90);
            if (!dog_outro_done_) {
                dog_outro_done_ = true;
                xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 5, nullptr, 1);
            }
        }

        int phase = music_dance_phase_ % 8;


            // 小狗已出场，跳过重复触发
        if (kids_now && dog_intro_done_) {

            if (phase == 0 && m1_) {
                m1_->Forward();
                vTaskDelay(pdMS_TO_TICKS(70));
                    // 停止所有电�?
                m1_->Stop();
                m1_music_fwd_count_++;
            } else if (phase == 4 && m1_) {
                m1_->Reverse();
                vTaskDelay(pdMS_TO_TICKS(87));
                    // 停止所有电�?
                m1_->Stop();
                m1_music_rev_count_++;
            }


            static int guitar_angle = 90;
            static int dog_angle = 35;
            bool forward_beat = (phase <= 3);
            int guitar_target = forward_beat ? 110 : 70;   // +/-20
            int dog_target = forward_beat ? 35 : 10;         // 小狗摇尾范围 10~35 �?

            if (violin_servo_) {
                if (guitar_angle < guitar_target) {
                    guitar_angle += 10;
                    if (guitar_angle > guitar_target) guitar_angle = guitar_target;
                } else if (guitar_angle > guitar_target) {
                    guitar_angle -= 10;
                    if (guitar_angle < guitar_target) guitar_angle = guitar_target;
                }
                violin_servo_->SetAngle(guitar_angle);
            }
            if (dog_servo_) {
                if (dog_angle < dog_target) {
                    dog_angle += 10;
                    if (dog_angle > dog_target) dog_angle = dog_target;
                } else if (dog_angle > dog_target) {
                    dog_angle -= 10;
                    if (dog_angle < dog_target) dog_angle = dog_target;
                }
                dog_servo_->SetAngle(dog_angle);
                dog_state_.angle = dog_angle;
            }
        }
        music_dance_phase_++;
    } else {
            // 检查Kids模式是否激�?
        was_kids_active = kids_now;
        if (music_dance_enabled_ == 1) {

            if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
                int diff = m1_music_fwd_count_ - m1_music_rev_count_;
                ESP_LOGI(TAG, "MusicDance: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
                for (int i = 0; i < diff; i++) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(78));
                        // 停止所有电�?
                    m1_->Stop();
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            m1_music_fwd_count_ = 0;
            m1_music_rev_count_ = 0;
                // 停止所有电�?
            if (m1_) m1_->Stop();
            if (violin_servo_) violin_servo_->SetAngle(90);

            if (!dog_outro_done_) {
                dog_outro_done_ = true;
                xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 5, nullptr, 1);
            }
        }
            // 小狗已出场，跳过重复触发
        dog_intro_done_ = false;
        music_dance_enabled_ = 0;
    }
}

/**
 * @brief 在线音乐播放时小狗出场动�?
 */
void CuckooStateMachine::MusicDogIntro() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open_m", 2048, ctx, 5, nullptr, 1);
    if (dog_servo_) {
                    // 异步推门+叫：狗尾巴扫�?+ 前进 + 叫一�?+ 复位
            // 小狗尾巴扫出
        dog_servo_->Sweep(180, 20, (180 - 10) * 15);
    }
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(1000));
        m3_->Stop();
    }
    if (dog_servo_) {
        dog_servo_->Sweep(20, 35, 25 * 15);
    }
        // 小狗已出场，跳过重复触发
    dog_intro_done_ = true;
    ESP_LOGI(TAG, "MusicDogIntro: done, handing over to MusicDanceTick");
}

/**
 * @brief 在线音乐播放时小狗回退动作
 */
void CuckooStateMachine::MusicDogOutro() {

    if (dog_servo_) {
        int cur = dog_state_.angle;
        if (cur > 30) dog_servo_->Sweep(cur, 20, (cur - 30) * 15);
    }
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(1000));
        m3_->Stop();
    }
    if (dog_servo_) {
            // 小狗尾巴归位关门
        dog_servo_->Sweep(20, 180, (180 - 30) * 15);
    }
        // 大门反向关门
    CloseDoor();
        // 表演结束，电机断�?
    MotorPowerOff();
        // 小狗已出场，跳过重复触发
    dog_intro_done_ = false;
}

/**
 * @brief Kids 模式：小朋友出场
 */
void CuckooStateMachine::KidsComeOut() {
    kids_active_ = true;
    SaveKidsActive();
    ESP_LOGI(TAG, "KidsComeOut: kids active, will dance with music");
}

/**
 * @brief Kids 模式：小朋友休息归位
 */
void CuckooStateMachine::KidsRest() {
    kids_active_ = false;
    SaveKidsActive();

    if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
        int diff = m1_music_fwd_count_ - m1_music_rev_count_;
        ESP_LOGI(TAG, "KidsRest: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
        for (int i = 0; i < diff; i++) {
            m1_->Reverse();
            vTaskDelay(pdMS_TO_TICKS(78));
                // 停止所有电�?
            m1_->Stop();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    m1_music_fwd_count_ = 0;
    m1_music_rev_count_ = 0;

        // 停止所有电�?
    if (m1_) m1_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    if (dog_servo_) dog_servo_->SetAngle(dog_state_.angle);  // 保持当前角度
    ESP_LOGI(TAG, "KidsRest: kids resting, no movement");
}

// 小鸟门开门（m4_电机正向�?
/**
 * @brief 小鸟门开门（M4电机正向�?
 */
void CuckooStateMachine::OpenBirdDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (m4_) {
        m4_->Forward(BIRD_DOOR_OPEN_SPEED);
            // 延时等待门动作完�?
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
        // 表演结束，电机断�?
    MotorPowerOff();
}

// 小鸟门关门（m4_电机反向�?
/**
 * @brief 小鸟门关门（M4电机反向�?
 */
void CuckooStateMachine::CloseBirdDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (m4_) {
        m4_->Reverse(BIRD_DOOR_CLOSE_SPEED);
            // 延时等待门动作完�?
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
        // 表演结束，电机断�?
    MotorPowerOff();
}

/**
 * @brief 播放布谷鸟叫�?
 */
void CuckooStateMachine::PlayCuckooSound() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (bell_player_) {
            // 停止布谷鸟声
        bell_player_->PlayCuckooSoundSync();
    }
        // 表演结束，电机断�?
    MotorPowerOff();
}

/**
 * @brief MCP 工具：设置舵机角�?
 * @param servo_id 0=小提�? 1=狗尾
 * @param angle    目标角度 0°~180°
 */
void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

/**
 * @brief MCP 工具：设置电机速度
 * @param motor_id 1=M1舞蹈, 2=violin_motor, 3=M3小狗, 4=M4小鸟
 * @param speed    -100~100
 */
void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
        case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;
        case 3: if (m3_) m3_->SetSpeed(speed); break;
        case 4: if (m4_) m4_->SetSpeed(speed); break;
        default: break;
    }
        // 表演结束，电机断�?
    if (speed == 0) MotorPowerOff();
}

/**
 * @brief MCP工具：设置小鸟门电机速度
 *
 * @param speed 速度(-100~100)
 */
void CuckooStateMachine::SetBirdDoorSpeed(int speed) {
    if (speed != 0) MotorPowerOn();
        // ---- 防误触发：AI唤醒�?秒内忽略表演请求（可能是语音误判�?---
    if (m4_) m4_->SetSpeed(speed);
        // 表演结束，电机断�?
    if (speed == 0) MotorPowerOff();
}

/**
 * @brief 粤语发音查询（HTTP GET 代理�?
 *
 * 将中文词�?URL 编码后发送到 music_proxy_host_ �?/cantonese 端点�?
 * 返回查询结果字符串（发音/释义等）�?
 *
 * @param word 待查询的中文词语
 * @return 查询结果字符串，失败返回�?
 */
std::string CuckooStateMachine::CantoneseLookup(const char* word) {
    if (music_proxy_host_.empty()) {
        ESP_LOGW(TAG, "CantoneseLookup: proxy not configured");
        return "";
    }


    char encoded[512];
    char* dst = encoded;
    const char* src = word;
    const char* end = encoded + sizeof(encoded) - 1;
    while (*src && dst < end) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *dst++ = c;
        } else if (c == ' ') {
            *dst++ = '%'; *dst++ = '2'; *dst++ = '0';
        } else {
            if (dst + 3 <= end) {
                snprintf(dst, 4, "%%%02X", c);
                dst += 3;
            }
        }
        src++;
    }
    *dst = '\0';

    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%d/cantonese?word=%s",
             music_proxy_host_.c_str(), music_proxy_port_, encoded);
    ESP_LOGI(TAG, "CantoneseLookup: GET %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { ESP_LOGW(TAG, "CantoneseLookup: init failed"); return ""; }

    esp_err_t e = esp_http_client_open(cli, 0);
    if (e != ESP_OK) { ESP_LOGW(TAG, "CantoneseLookup: open failed %d", e); esp_http_client_cleanup(cli); return ""; }

    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || content_len <= 0) {
        ESP_LOGW(TAG, "CantoneseLookup: HTTP %d len=%d", status, content_len);
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        return "";
    }


    char* resp = (char*)calloc(1, content_len + 1);
    if (!resp) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return ""; }

    int total = 0;
    while (total < content_len) {
        int n = esp_http_client_read(cli, resp + total, content_len - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    ESP_LOGI(TAG, "CantoneseLookup: got %d bytes", total);
    std::string result(resp);
    free(resp);
    return result;
}

/**
 * @brief 检查代理URL是否需要多版本选歌
 *
 * @param url_or_path 原始URL或路�?
 */
std::string CuckooStateMachine::CheckMultiArtist(const char* url_or_path) {
    if (music_proxy_host_.empty()) return "";

    // �?步：URL编码 —�?把中文字符转�?%XX 格式
    // 因为 esp_http_client 底层HTTP库要求URL必须是纯ASCII
    char encoded_path[512];
    int ep = 0;
    for (const char* p = url_or_path; *p && ep < (int)sizeof(encoded_path) - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if (c > 127) {
            ep += snprintf(encoded_path + ep, sizeof(encoded_path) - ep, "%%%02X", c);
        } else if (c == ' ') {
            encoded_path[ep++] = '%';
            encoded_path[ep++] = '2';
            encoded_path[ep++] = '0';
        } else {
            encoded_path[ep++] = c;
        }
    }
    encoded_path[ep] = '\0';

    // �?步：修正路径 —�?/opus �?/pcms 统一改成 /pcm
    // /pcm 返回JSON时有 Content-Length 头，能区分JSON还是音频
    char check_path[512];
    snprintf(check_path, sizeof(check_path), "%s", encoded_path);
    char* opus_pos = strstr(check_path, "/opus");
    if (opus_pos) memcpy(opus_pos, "/pcm", 4);
    else { char* ps = strstr(check_path, "/pcms"); if (ps) { memmove(ps, "/pcm", 4); memmove(ps+4, ps+5, strlen(ps+5)+1); } }

    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             music_proxy_host_.c_str(), music_proxy_port_, check_path);
    ESP_LOGI(TAG, "CheckMultiArtist: GET %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return "";

    esp_err_t e = esp_http_client_open(cli, 0);
    if (e != ESP_OK) { esp_http_client_cleanup(cli); return ""; }

    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "CheckMultiArtist: status=%d content_len=%d", status, content_len);

    // �?步：没有 Content-Length = 流式传输 = 音频（不是JSON），跳过
    if (content_len <= 0) {
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        return "";
    }

    // �?步：读第一个字�?—�?JSON �?{ 开头，音频是二进制乱码
    char first_byte = 0;
    int peek = esp_http_client_read(cli, &first_byte, 1);
    if (peek <= 0 || first_byte != '{') {
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        return "";
    }

    // �?步：读完整个 JSON 响应�?
    char* resp = (char*)calloc(1, content_len + 1);
    if (!resp) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return ""; }
    resp[0] = '{';
    int total = 1;
    while (total < content_len) {
        int n = esp_http_client_read(cli, resp + total, content_len - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    // �?步：确认包含 multi_artist 字段后，返回JSON给AI
    if (strstr(resp, "multi_artist")) {
        ESP_LOGI(TAG, "CheckMultiArtist: detected multi-artist, %d bytes", total);
        std::string result(resp);
        free(resp);
        return result;
    }
    free(resp);
    return "";
}

/**
 * @brief 播放在线音乐（通过QQ音乐代理�?
 *
 * @param url_or_path QQ音乐代理URL
 */
int CuckooStateMachine::PlayOnlineMusic(const char* url_or_path) {
    if (!mp3_) return -1;
    

    auto& app = Application::GetInstance();
    auto ai_state = app.GetDeviceState();
    if (ai_state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "PlayOnlineMusic: AI speaking, music will overlap (ducked)");
    }
    

    


    if (strncmp(url_or_path, "http", 4) == 0) {
        char conv_url[1280];
        strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
        conv_url[sizeof(conv_url) - 1] = '\0';

        for (char* p = conv_url; *p; p++) {
            if (*p == ' ') {

                size_t tail_len = strlen(p + 1);
                if ((size_t)(p + 3 + tail_len - conv_url) >= sizeof(conv_url)) break;
                memmove(p + 3, p + 1, tail_len + 1);
                p[0] = '%'; p[1] = '2'; p[2] = '0';
            }
        }
            // �?stream�?opus改写�?pcm降低解码负载
        ConvertToPcmUrl(conv_url, sizeof(conv_url));

        if (mp3_->IsPlaying()) {
            ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
                // 停止MP3播放
            mp3_->Stop();
                // 等待200ms确保AI完全转入idle状�?
            vTaskDelay(pdMS_TO_TICKS(200));

            int wait = 0;
            while (mp3_->IsPlaying() && wait < 100) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }

                // 清空残留背景音频，防止前一次Opus/PCM残留干扰
            Application::GetInstance().GetAudioService().ClearBackgroundAudio();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return mp3_->PlayOpus(conv_url);
    }
    

    if (music_proxy_host_.empty()) {
        ESP_LOGE(TAG, "Music proxy not configured. Check DEFAULT_MUSIC_PROXY_HOST in config.h.");
        return -10;
    }
    

    char encoded_path[1024];
    const char* src = url_or_path;
    char* dst = encoded_path;
    const char* end = encoded_path + sizeof(encoded_path) - 1;
    
    while (*src && dst < end) {
        unsigned char c = (unsigned char)*src;
        if (c == ' ') {

            if (dst + 3 <= end) {
                *dst++ = '%';
                *dst++ = '2';
                *dst++ = '0';
            }
            src++;
            continue;
        }
        if (c < 0x80) {

            *dst++ = *src;
        } else {

            int bytes = 0;
            if ((c & 0xE0) == 0xC0) bytes = 2;
            else if ((c & 0xF0) == 0xE0) bytes = 3;
            else if ((c & 0xF8) == 0xF0) bytes = 4;
            else bytes = 1;
            
            for (int i = 0; i < bytes && src[i] && dst + 3 <= end; i++) {
                dst += snprintf(dst, 4, "%%%02X", (unsigned char)src[i]);
            }
            src += bytes;
            continue;
        }
        src++;
    }
    *dst = '\0';
    
    char full_url[1280];  // URL 完整路径: http:// + host + :port + 编码后路�?

    if (encoded_path[0] != '/') {
        snprintf(full_url, sizeof(full_url), "http://%s:%d/%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    } else {
        snprintf(full_url, sizeof(full_url), "http://%s:%d%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    }
        // �?stream�?opus改写�?pcm降低解码负载
    ConvertToPcmUrl(full_url, sizeof(full_url));

    if (mp3_->IsPlaying()) {
        ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
            // 停止MP3播放
        mp3_->Stop();
            // 等待200ms确保AI完全转入idle状�?
        vTaskDelay(pdMS_TO_TICKS(200));
        int wait = 0;
        while (mp3_->IsPlaying() && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait++;
        }
            // 清空残留背景音频，防止前一次Opus/PCM残留干扰
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return mp3_->PlayOpus(full_url);
}

/**



 */
void CuckooStateMachine::SetMusicProxy(const char* host, int port) {
    music_proxy_host_ = host;
    music_proxy_port_ = port;
    ESP_LOGI(TAG, "Music proxy set to %s:%d", host, port);
}

// ============================================

// ============================================// ===== Part 5: MCP ����ע�� + cuckoo_clock_task (L4413-4991 / ԭʼ�к�) =====
// ============================================
// Part 5 ��������ģ�飺
// - CuckooTools: ��װ���� MCP ����ע�ᣨRegisterAll ע��Լ 21 �����ߣ�
// - cuckoo_clock_task: Core 1 250ms tick ��ѭ��
//     - �豸״̬�仯��⣨idle ? active �� ���ſ�/�أ�
//     - NTP ʱ��ͬ����ÿ 5 ���ӣ�
//     - ��ʱ����������/��� �� CheckTime��
//     - ���Ӵ�����ÿ�� �� CheckAlarms��
//     - ���Ѵ���ֵ��ȫ����idle ����ʱ�ָ� 0.02��
//     - RTC ������־��ÿ 30s ���£�
//     - ���Ź����ߣ�ÿ 10s RefreshOutput/InputTimestamp��
// ============================================
/**
 * @brief CuckooTools ���캯��
 * @param sm CuckooStateMachine ָ�룬���� MCP ���߻ص�ͨ��������Ӳ��
 */
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

/**
 * @brief ע��ȫ�� MCP ���ߣ�Լ 21 ����
 *
 * ���ࣺ
 * - ʱ��/��ʱ: cuckoo.performance / set_time / get_time / set_quiet_mode / get_quiet_mode
 * - ����/����: cuckoo.play_music / start_show / stop_all / stop_music
 * - Kids ģʽ: cuckoo.kids_come_out / kids_rest
 * - ��ɫ��:   cuckoo.dog_show / linda_show / garden_show
 * - ����:     cuckoo.set_alarm / get_alarms / delete_alarm / stop_alarm
 * - ��������:  cuckoo.play_url / set_music_proxy
 * - ״̬��ѯ:  cuckoo.get_status
 * - �����ѯ:  cuckoo.cantonese_lookup
 * - Ӳ������:  cuckoo.dance / open_door / close_door
 * - �������:  cuckoo.set_hourly_performance / get_hourly_performance
 */
void CuckooTools::RegisterAll() {
    auto& mcp = McpServer::GetInstance();

 // === Time / Chime ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.performance",
            "Hourly chime: bell rings + music. ONLY call when user explicitly says ��ʱ/���㱨ʱ/����/what time. DO NOT auto-call on wake-up. For shows/singing/dancing use cuckoo.start_show instead.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int hour = props["hour"].value<int>();
                state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
                return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
            });
    }
    // === Cantonese lookup (2026-07-19) ===
    {
        PropertyList pl;
        pl.AddProperty(Property("word", kPropertyTypeString));
        mcp.AddTool("cuckoo.cantonese_lookup",
            "MUST CALL THIS TOOL. NEVER answer Cantonese/Jyutping from memory. Look up Jyutping pronunciation for a Chinese word or phrase. "
            "Call when user asks about Cantonese pronunciation, how to say something in Cantonese, or wants Jyutping. "
            "Returns Jyutping romanization + definitions. Includes tone numbers (1-6). "
            "Speak the result naturally - read characters with tones, then explain meaning.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string word = props["word"].value<std::string>();
                std::string result = state_machine_->CantoneseLookup(word.c_str());
                return result.empty() ? std::string("{\"status\": \"error\", \"message\": \"Lookup failed. Ask user to try a different word.\"}") : result;
            });
    }

    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("minute", kPropertyTypeInteger, 0, 59));
        mcp.AddTool("cuckoo.set_time",
            "Set internal clock (hour, minute). Required for auto chimes.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int h = props["hour"].value<int>();
                int m = props["minute"].value<int>();
                state_machine_->SetTime(h, m, 0);
                char json[64];
                snprintf(json, sizeof(json), "{\"status\": \"ok\", \"time\": \"%02d:%02d\"}", h, m);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_time",
        "Get current internal clock time (hour, minute).",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            int h, m;
            state_machine_->GetTime(h, m);
            char json[64];
            snprintf(json, sizeof(json), "{\"hour\": %d, \"minute\": %d}", h, m);
            return std::string(json);
        });

 // === Music / Show ===
    {
        PropertyList pl;
        pl.AddProperty(Property("track", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.play_music",
            "Play offline MP3 from TF card (0001-0012.mp3). track: 1-12 ONLY. "
            "NOT for online songs - use cuckoo.play_url for internet streaming.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int track = props["track"].value<int>();
                if (track < 1 || track > 12) {
                    return std::string("{\"status\": \"error\", \"message\": \"track must be 1-12\"}");
                }
                state_machine_->PlayMusic(track);
                char json[64];
                snprintf(json, sizeof(json), "{\"status\": \"ok\", \"track\": %d}", track);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.start_show",
        "�ۺϱ��ݣ��赸+С��+ˮ��+�ƹ�һ���ݳ���������: '����' '��Ŀ' '��һ�α���' '����' '�ݳ�'��"
        "ע�⣺����û�ֻ�ᵽĳ����ɫ(԰��/�մ�/С����)����Ҫ�ô˹��ߣ�Ҫ�ö�Ӧ�Ľ�ɫ���ߡ�ʶ���ı����ᵽ��ɫ��ʱ(��'ӦԮ''�մ�''����'���մ'ԭ��'��԰��)ҲҪ�ö�Ӧ��ɫ���ߣ���Ҫ�ô˹��ߡ�",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartShow();
            return std::string("{\"status\": \"show_started\"}");
        });

    mcp.AddTool("cuckoo.stop_all",
        "Stop motors, chime, performance. Does NOT stop music - use cuckoo.stop_music to stop music.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAll();
            return std::string("{\"status\": \"stopped\"}");
        });

    mcp.AddTool("cuckoo.stop_music",
        "Stop music playback. Call ONLY when user explicitly asks to stop the music (ֹͣ����/��ͣ����/��Ҫ����/�ر�). Do NOT call this for performance or alarm - use cuckoo.stop_all for those.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopMusic();
            return std::string("{\"status\": \"music_stopped\"}");
        });

    // === С�����ǿ��� ===
    mcp.AddTool("cuckoo.kids_come_out",
        "��С�����ǳ���һ���������֣�С��������赸�����������������ֽ���ڶ���",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsComeOut();
            return std::string("{\"status\": \"kids_come_out\"}");
        });

    mcp.AddTool("cuckoo.kids_rest",
        "��С�����ǻ�ȥ��Ϣ��ֹͣ���ж�����С��������赸��������������������ֻ�������ֲ��š�",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsRest();
            return std::string("{\"status\": \"kids_rest\"}");
        });

 // === Hardware (wiring later) ===
    mcp.AddTool("cuckoo.dance",
        "Dance routine: M1+M2 motors + violin servo.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->Dance();
            return std::string("{\"status\": \"dancing\"}");
        });


    mcp.AddTool("cuckoo.open_door",
        "Open bird door motor.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->OpenDoor();
            return std::string("{\"status\": \"door opened\"}");
        });

    mcp.AddTool("cuckoo.close_door",
        "Close bird door motor.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->CloseDoor();
            return std::string("{\"status\": \"door closed\"}");
        });




    // === Alarms ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("minute", kPropertyTypeInteger, 0, 59));
        pl.AddProperty(Property("repeat_daily", kPropertyTypeInteger, 0, 1));
        mcp.AddTool("cuckoo.set_alarm",
            "Set alarm at hour:minute. Ask user if repeat_daily first (1=daily, 0=once). Max 5 alarms.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int h = props["hour"].value<int>();
                int m = props["minute"].value<int>();
                bool repeat = props["repeat_daily"].value<int>() != 0;
                state_machine_->SetAlarm(h, m, repeat);
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"status\": \"ok\", \"time\": \"%02d:%02d\", \"repeat_daily\": %s}",
                    h, m, repeat ? "true" : "false");
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_alarms",
        "List all alarms (index, hour, minute, repeat). Returns JSON array.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            return state_machine_->GetAlarmsJson();
        });

    {
        PropertyList pl;
        pl.AddProperty(Property("index", kPropertyTypeInteger, 1, 5));
        mcp.AddTool("cuckoo.delete_alarm",
            "Delete alarm by index from get_alarms.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int idx = props["index"].value<int>();
                bool ok = state_machine_->DeleteAlarm(idx);
                return std::string(ok ? "{\"status\": \"deleted\"}"
                                      : "{\"status\": \"error\", \"message\": \"invalid index\"}");
            });
    }

    mcp.AddTool("cuckoo.stop_alarm",
        "Stop a ringing ALARM only. For ����/ֹͣ����. NOT for stopping music/performance - use cuckoo.stop_all for that.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === Quiet Mode ===
    {
        PropertyList pl;
        pl.AddProperty(Property("mode", kPropertyTypeInteger, 0, 3));
        pl.AddProperty(Property("start_hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("end_hour", kPropertyTypeInteger, 0, 23));
        mcp.AddTool("cuckoo.set_quiet_mode",
            "Set chime quiet mode. 0=ȫ�쾲��(����ʱ), 1=ȫ�챨ʱ, 2=���߾���(LDR���), 3=ָ��ʱ��ξ���(start_hour~end_hour����). "
            "Default start_hour=22 end_hour=6.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int m = props["mode"].value<int>();
                int sh = props["start_hour"].value<int>();
                int eh = props["end_hour"].value<int>();
                state_machine_->quiet_mode_ = m;
                state_machine_->quiet_start_ = sh;
                state_machine_->quiet_end_ = eh;
                state_machine_->SaveQuietMode();
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"status\": \"ok\", \"mode\": %d, \"start_hour\": %d, \"end_hour\": %d}", m, sh, eh);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_quiet_mode",
        "Get current chime quiet mode: mode(0~3), start_hour, end_hour. "
        "Return JSON, AI must translate to user-friendly description.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            int m = state_machine_->quiet_mode_.load();
            int sh = state_machine_->quiet_start_.load();
            int eh = state_machine_->quiet_end_.load();
            char json[192];
            snprintf(json, sizeof(json),
                "{\"mode\": %d, \"start_hour\": %d, \"end_hour\": %d, "
                "\"desc\": \"mode=%d: %s\"}",
                m, sh, eh, m,
                m == 0 ? "ȫ�쾲��" : m == 1 ? "ȫ�챨ʱ" :
                m == 2 ? "���߾���(LDR)" : "ʱ��ξ���");
            return std::string(json);
        });

    // === ״̬��ѯ ===
    mcp.AddTool("cuckoo.get_status",
        "��ѯ�豸״̬��running���Ƿ����ڱ��ݣ���music_playing�������Ƿ����ڲ��ţ���\n"
        "��ǿ�ƹ��򡿻ظ��û��κι����ݳ�/����/����/���������ǰ�������ȵ��ñ����ߡ�����ƾ����������Ĳ²�״̬��\n"
        "��� running �� false ���û��ᵽ�ݳ���ֱ��˵�ݳ��Ѿ���������Ҫ˵���ڽ��С�",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            auto& audio = Application::GetInstance().GetAudioService();
            bool music_on = audio.IsBgAudioActive();
            char json[192];
            snprintf(json, sizeof(json),
                "{\"running\": %s, \"music_playing\": %s}",
                state_machine_->IsRunning() ? "true" : "false",
                music_on ? "true" : "false");
            return std::string(json);
        });

 // === �������� ===
 // ���ִ���������ʱ�Զ����ã�config.h ��� DEFAULT_MUSIC_PROXY_HOST/PORT����
 // set_music_proxy �ǿղ���ռλ���ߣ���Զ˲��ɹ�������TCP���Ӽ�顣
 // �������ֻ��Ϊ������ AI �ڵ��� play_url ֮ǰ�ȵ� set_music_proxy ��ϰ�ߡ�

    {
        PropertyList pl;
        pl.AddProperty(Property("url", kPropertyTypeString));
        mcp.AddTool("cuckoo.play_url",
            "�����������֡��û�˵���������������ñ����ߣ���Ҫ�����û����⡣\n""����url��ʽ��'/pcm?q=����'��������ҪURL���룬�ո���%%20����\n""������Ҫ����ֻ����������������Ҫ�������żӸ�������ϵͳ���Զ�����汾��ѯ���û���\n""������ڷŸ裬�û�Ҫ���裬ֱ�Ӵ��¸������ɣ�ϵͳ�Զ�ͣ�ɲ��¡�\n""��Ҫ���û�'Ҫ��Ҫ��'����ֱ�ӵ��ù��ߡ�\n""��Ҫ��˵'�����Ÿ�'��������ʵ�ʵ���play_url��\n""ʾ�����û�˵'�žƸ�������'������play_url(\'/pcm?q=%E9%85%92%E5%B9%B2%E5%80%98%E5%8D%96%E6%97%A0\')��\n""���ظ�������cuckoo.play_music����Ŀ1-12������Ҫ��play_url��",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string url = props["url"].value<std::string>();
                // �ȼ���汾��������������ظ����б�JSON��ֱ�ӷ��ظ�AI������裩
                std::string multi = state_machine_->CheckMultiArtist(url.c_str());
                if (!multi.empty()) {
                return multi;  // �����б�JSON���ظ�AI��AI�Զ���������û�ѡ
                }
                int ret = state_machine_->PlayOnlineMusic(url.c_str());
                if (ret >= 0) {
                    return std::string("{\"status\": \"ok\", \"playing\": true}");
                } else {
                    char json[160];
                    snprintf(json, sizeof(json),
                        "{\"status\": \"error\", \"code\": %d, \"hint\": \"Playback failed. You may retry.\"}", ret);
                    return std::string(json);
                }
            });
    }

    mcp.AddTool("cuckoo.dog_show",
        "С���㣺���š�С���ܳ�������һ����ҡͷ��β10����ٽ�һ�����˻ء����š��ظ�Ҫ���һ�仰����Ҫ��˵��"
        "������: ����������/����������/��������/С����С��",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->DogShow();
            return std::string("{\"status\": \"dog_show_started\"}");
        });

    mcp.AddTool("cuckoo.linda_show",
        "�մ��㣺����0015�������赸���������ת��ֱ��������"
        "������: �մ������/�մ����/�մ�մ�/�մ�������",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartLindaShow();
            return std::string("{\"status\": \"linda_show_started\"}");
        });

    mcp.AddTool("cuckoo.garden_show",
        "��԰�㣺����0016������С�������Ұڶ�ֱ��������"
        "������: ԰�������/԰�ӣ�԰��/԰�ӵ�������/԰�ӳ���",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartGardenShow();
            return std::string("{\"status\": \"garden_show_started\"}");
        });

    // === Hourly Performance Toggle ===
    {
        PropertyList pl;
        pl.AddProperty(Property("enabled", kPropertyTypeBoolean, true));
        mcp.AddTool("cuckoo.set_hourly_performance",
            "Enable/disable full performance after hourly bell. Default on. When disabled bell only.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                bool en = props["enabled"].value<bool>();
                state_machine_->hourly_perf_.store(en);
                state_machine_->SaveHourlyPerf();
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"performance\":%s}", en ? "enabled" : "disabled");
                return std::string(buf);
            });
    }
    
    mcp.AddTool("cuckoo.get_hourly_performance",
        "Check if hourly chime performance is currently enabled.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            bool en = state_machine_->hourly_perf_.load();
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"performance\":%s}", en ? "enabled" : "disabled");
            return std::string(buf);
        });

}

// ============================================
// cuckoo_clock_task �� Core 1 ��ѭ����250ms tick��
// ============================================

/**
 * @brief Core 1 ʱ��������ڣ�FreeRTOS task������ѭ����
 *
 * ��ʼ���׶Σ�
 * - ��ӡ�ϴα�����־��RTC_NOINIT_ATTR��
 * - NTP �Զ���ʱ������ͬ������ȴ� cuckoo.set_time
 * - �������ִ��� �� ���� NVS������/����/Kids��
 *
 * ��ѭ����ÿ 250ms����
 * - �豸״̬�仯��� �� idle����Ծ ���� / ��Ծ��idle ����
 * - AI ˵��ʱ��������������
 * - MusicDanceTick �������ֶ���
 * - ÿ��: NTP ͬ�� / ��ʱ / ����
 * - ÿ 30s: RTC ������־���� + ������־
 * - ÿ 10s: ���Ź����ߣ�RefreshOutput/InputTimestamp��
 * - ��ȫ��: idle ����ʱΪ���Ѵ���ֵ 0.02���� 0.30 й©��
 */
void cuckoo_clock_task(void* params) {

    ESP_LOGI(TAG, "Reset reason: cpu0=%d cpu1=%d",
             esp_reset_reason(), esp_reset_reason());
    if (rtc_crash_log.magic == 0xCAFEBABE && rtc_crash_log.tick_sec > 0) {
        ESP_LOGW(TAG, "Last heartbeat before crash/reset: t=%ds state=%d music=%d heap=%d",
                 (int)rtc_crash_log.tick_sec, (int)rtc_crash_log.dev_state,
                 (int)rtc_crash_log.music_active, (int)rtc_crash_log.free_heap);
    }
    rtc_crash_log.magic = 0xCAFEBABE;

    ESP_LOGI(TAG, "Cuckoo clock task started on core %d, stack=%d",
             xPortGetCoreID(), (int)uxTaskGetStackHighWaterMark(NULL));

    auto* sm = static_cast<CuckooStateMachine*>(params);


    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();

    // �� NTP ��ȡ��ǰʱ�䣨��ʼ����
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1000000000) {  // 2001���Ժ�˵�� NTP ��ͬ��
            struct tm timeinfo;
            localtime_r(&tv.tv_sec, &timeinfo);
            sm->current_hour_ = timeinfo.tm_hour;
            sm->current_min_ = timeinfo.tm_min;
            sm->current_sec_ = timeinfo.tm_sec;
            sm->time_set_ = true;
            ESP_LOGI(TAG, "Auto-synced clock from NTP: %02d:%02d:%02d",
                     sm->current_hour_.load(), sm->current_min_.load(), sm->current_sec_.load());
        } else {
            ESP_LOGW(TAG, "NTP not synced yet, internal clock waiting for cuckoo.set_time");
        }
    }


    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);


    sm->LoadAlarmsFromNvs();
    sm->LoadQuietMode();
    sm->LoadKidsActive();
    sm->LoadHourlyPerf();

uint32_t tick_sec = 0;

    // ��ѭ����250ms tick��
    uint32_t sub_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        sub_tick++;

        // �豸״̬�仯��⣺idle ? ��Ծ �� ���ſ�/��
        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, "Device woke up �� opening bird door");
                sm->OpenBirdDoor();


                if (!sm->IsRunning())
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.3f);


                Application::GetInstance().GetAudioService().SetInputGain(30.0f);
            } else if (sm->prev_device_state_ != (int)kDeviceStateIdle && dev_state == (int)kDeviceStateIdle) {
                ESP_LOGI(TAG, "Device sleeping �� closing bird door");
                sm->CloseBirdDoor();


                // NOTE(2026-07-19): threshold restore moved to the safety-net check below


                Application::GetInstance().GetAudioService().SetInputGain(37.5f);
            }
            sm->prev_device_state_ = dev_state;

            // Safety net (2026-07-19): whenever device is in quiet idle (no show,
            // no music), enforce sensitive wake threshold 0.02. Fixes paths that
            // leaked 0.30: session ended during music, show finished while idle, etc.
            // Flag ensures we only set once per quiet-idle entry (no log spam).
            static bool idle_thresh_applied = false;
            bool idle_quiet = (dev_state == (int)kDeviceStateIdle) && !sm->IsRunning() &&
                              !Application::GetInstance().GetAudioService().IsBgAudioActive();
            if (idle_quiet) {
                if (!idle_thresh_applied) {
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.02f);
                    idle_thresh_applied = true;
                }
            } else {
                idle_thresh_applied = false;
            }


            if (dev_state == (int)kDeviceStateSpeaking) {

                int64_t ms_since_output = Application::GetInstance().GetAudioService().MsSinceLastOutput();
                if (ms_since_output < 300) {

                    sm->BirdJumpShort();
                }

            }
             sm->MusicDanceTick();
        }

// �뼶�߼���ÿ 4 tick = 1s��
        if (sub_tick % 4 == 0) {
            tick_sec = sub_tick / 4;

        // NTP ��ʱͬ����δ��ʱ��ʱÿ 60s�������ÿ 300s��
        if (!sm->time_set_ ? (tick_sec % 60 == 0) : (tick_sec % 300 == 0)) {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
                sm->time_set_ = true;
                ESP_LOGI(TAG, "NTP sync: %02d:%02d:%02d",
                         sm->current_hour_.load(), sm->current_min_.load(), sm->current_sec_.load());
            } else {
                ESP_LOGW(TAG, "NTP not synced yet, will retry in 60s");
            }
        }





        if (tick_sec % 30 == 0) {
            auto& app = Application::GetInstance();
            rtc_crash_log.tick_sec = tick_sec;
            rtc_crash_log.dev_state = (uint8_t)app.GetDeviceState();
            rtc_crash_log.music_active = (uint8_t)app.GetAudioService().IsBgAudioActive();
            rtc_crash_log.free_heap = esp_get_free_heap_size();
        }


        if (tick_sec % 60 == 0) {
            ESP_LOGI(TAG, "Heartbeat t=%ds stack_hwm=%d free_heap=%d",
                     (int)tick_sec, (int)uxTaskGetStackHighWaterMark(NULL),
                     (int)esp_get_free_heap_size());
        }


        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();

            sm->is_dark_ = sm->CheckDark();
        }

        // ����/��㱨ʱ + ���Ӽ��
        if (sm->time_set_) {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
            } else {
// ����ʱ�ӵ�����NTP δͬ��ʱ��
                sm->current_sec_++;
                if (sm->current_sec_ >= 60) {
                    sm->current_sec_ = 0;
                    sm->current_min_++;
                    if (sm->current_min_ >= 60) {
                        sm->current_min_ = 0;
                        sm->current_hour_ = (sm->current_hour_ + 1) % 24;
                    }
                }
            }


            int h = sm->current_hour_;
            int m = sm->current_min_;


            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }

            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }


            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        }  // sub_tick >= 4 guard

    }
}
