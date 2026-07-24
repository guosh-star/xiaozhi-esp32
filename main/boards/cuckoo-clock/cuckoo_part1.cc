// ===== Part 1: 基础模块 (L1-283) - 包含、RTC、Motor、Servo =====
// ============================================
// 布谷鸟钟控制器 (Cuckoo Controller)
//
// 功能概要：
// - 电机驱动：4路直流电机 + 1路振动电机 + 水车/鸟跳
// - 舵机控制：小提琴手臂舵机、小狗尾巴舵机
// - LED 闪烁：2路 LED 开/关控制
// - 音频播放：支持 MP3/Opus/PCM 解码，HTTP 下载，Ducking 智能混音
// - 整点/半点报时：开门控制 + 小鸟跳跃 + 音乐演奏 + 钟声
// - 综合表演：舞蹈电机 + 小提琴 + 小狗 + 水车 + 鸟跳 + LED
// - 特色 MCP 工具：DogShow、LindaShow、GardenShow
// - 闹钟功能：NVS 持久化，最多5个，支持每天重复
// - 静音模式：支持夜间静音（22:00-6:00）
// - 在线音乐播放：QQ 音乐代理（Opus/PCM 格式下载）
// - MCP 工具注册（21个工具），通过 MCP 协议供 AI 大模型调用
//
// 双核架构：
// - Core 0: AI 语音（唤醒词、TTS、Opus 解码）
// - Core 1: 钟控（cuckoo_clock_task，250ms tick，时间同步和报时）
// - 表演/报时通过 xTaskCreatePinnedToCore 在 Core 1 执行
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


// RTC 崩溃日志（RTC_NOINIT_ATTR，上电不丢失，用于诊断重启原因）
RTC_NOINIT_ATTR struct {
    uint32_t magic;        // 魔数: 0xCAFEBABE = 数据有效
    uint32_t tick_sec;     // 重启前累计运行秒数
    uint8_t  dev_state;    // 重启前设备状态
    uint8_t  music_active; // 重启前音乐是否在播放
    uint32_t free_heap;    // 重启前剩余堆内存
} rtc_crash_log;


// 将 QQ 音乐代理 URL 的 /stream 或 /opus 改写为 /pcm 以降低 ESP32 解码负荷
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
// LEDC PWM 通道分配（共7路）
// ============================================
// TB6612 电机驱动：4路 PWM 驱动 (频率 ~10-100KHz)
// 舵机：2路 PWM 驱动 (频率 50Hz)
// 单向控制 L9110S: 1路 PWM 驱动 (频率 ~1-10KHz)


// ============================================
// 四路主电机 (TB6612 / DRV8833)：控制大门(M2)、小狗(M3)、小鸟门(M4)
// ============================================

// 通用 GPIO 初始化（支持直驱和 PWM 双模式）
static void motor_gpio_init(gpio_num_t in1, gpio_num_t in2) {
    gpio_config_t c = { .pin_bit_mask = (1ULL<<in1)|(1ULL<<in2),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}
/**
 * @brief 电机构造函数 - GPIO 直驱模式（非 PWM）
 *
 * 通过 GPIO 高低电平直接控制电机正反转，无速度调节。
 * in1 高 + in2 低 = 正转，in1 低 + in2 高 = 反转。
 * 适用于 L9110S 等单极性驱动芯片。
 *
 * @param in1 正转引脚
 * @param in2 反转引脚
 */
Motor::Motor(gpio_num_t in1, gpio_num_t in2)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(false), max_duty_(0) { motor_gpio_init(in1, in2); Stop(); }
/**
 * @brief 电机构造函数 - PWM 共享定时器模式（10-bit/1kHz）
 *
 * 使用 LEDC_TIMER_MOTOR 定时器（10-bit 分辨率，max_duty=1023，频率 1kHz）。
 * 定时器通过 static t0_done 全局只初始化一次，后续实例复用同一配置。
 * 两个 LEDC 通道 (ch1/ch2) 分别控制正反转占空比，SetSpeed 时根据速度正负选择通道输出。
 * 适用于 TB6612 四路主电机驱动。
 *
 * @param in1 正向 GPIO 引脚
 * @param in2 反向 GPIO 引脚
 * @param ch1 正向 LEDC 通道号
 * @param ch2 反向 LEDC 通道号
 * @param sm LEDC 速度模式（通常为 LEDC_LOW_SPEED_MODE）
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
 * @brief 电机构造函数 - PWM 独立定时器模式（8-bit/10kHz）
 *
 * 使用自定义 LEDC 定时器编号（8-bit 分辨率，max_duty=255，频率 10kHz）。
 * 通过 static custom_done[timer] 数组确保每个定时器编号只初始化一次，
 * 不同编号的电机可独立调速互不干扰。
 * 适用于水车、鸟跳等需要独立频率控制的电机（DRV8833 单向驱动）。
 *
 * @param in1 正向 GPIO 引脚
 * @param in2 反向 GPIO 引脚
 * @param ch1 正向 LEDC 通道号
 * @param ch2 反向 LEDC 通道号
 * @param timer LEDC 定时器编号（独立于 LEDC_TIMER_MOTOR）
 * @param sm LEDC 速度模式
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
/**
 * @brief 设置电机速度
 *
 * @param speed -100（全速反转）到 100（全速正转），0=停止
 * GPIO 直驱或 PWM 占空比，自动判断 use_pwm_。
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
/**
 * @brief 停止电机（制动）
 *
 * GPIO：两脚置低。PWM：两通道 duty 置零。
 */
void Motor::Stop() {
    if (use_pwm_) { ledc_set_duty(speed_mode_, ledc_channel_, 0); ledc_set_duty(speed_mode_, ledc_channel2_, 0);
        ledc_update_duty(speed_mode_, ledc_channel_); ledc_update_duty(speed_mode_, ledc_channel2_); }
    else { gpio_set_level(in1_pin_, 0); gpio_set_level(in2_pin_, 0); }
/**
 * @brief 电机正转
 *
 * @param speed 速度百分比（0~100），默认 100
 */
/**
 * @brief 电机反转
 *
 * @param speed 速度百分比（0~100），默认 100
 */
void Motor::Reverse(int speed) { SetSpeed(speed > 0 ? -speed : -100); }

// ============================================

// 舵机 (SG90)
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
/**
 * @brief 设置舵机角度 (0°~180°)
 *
 * @param angle 目标角度，自动钳位到 [0, 180]
 * 将角度转换为 50Hz PWM 占空比 (409~2048 对应 0°~180°)
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




/**
 * @brief 舵机平滑扫描
 *
 * @param from 起始角度
 * @param to 目标角度
 * @param duration_ms 持续时间（毫秒），每 20ms 一步
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
// 水车 + 鸟跳双向电机（DRV8833 单向控制 IN1/IN2）
// config.h 配置: MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP
// 水车旋转: SetSpeed(WATER_WHEEL_SPEED) 沿 IN1 正转
// 水车反转(制动): SetSpeed(-100) 沿 IN2 反转
// ============================================

// ============================================
// MP3 播放器
// ============================================
// 支持: DecodeSingleFile(本地MP3) / PlayUrl(HTTP流MP3) / PlayPcm(原始PCM) / PlayOpus(OGG/Opus)
// Ducking: AI 说话时自动将音乐音量从 100% 降至 20%（约 400ms），停止说话后渐恢复至 100%
// 狗叫混音: LoadDogBark 将预编译 PCM 加载到混音缓冲，在输出循环中叠加
// ============================================

/**
 * @brief 析构函数，停止播放并等待异步下载任务退出（5秒超时）
 */
Mp3Player::~Mp3Player() {
    stop_requested_ = true;
    // 等待最多5秒让下载线程退出（recv 返回 error 后关 socket）
    for (int i = 0; i < 100 && is_playing_; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  // 总共5秒超时
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
