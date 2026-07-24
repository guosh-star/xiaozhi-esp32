// ============================================
// 布谷鸟钟控制器 (Cuckoo Controller)

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


// 将 QQ 音乐代理 URL 的 /stream 或 /opus 改写为 /pcm 以降低 ESP32 解码负载
static void ConvertToPcmUrl(char* url, size_t url_sz) {
// 检查 URL 是否超出缓冲区上限
    size_t url_len = strlen(url);
    if (url_len >= url_sz) return;

    // /stream?q=... 转换为 /pcm?q=...（降低 ESP32 解码负荷）
    char* pos = strstr(url, "/stream?");
    if (pos) {
// "/stream" 占 7 字节，用 "/pcm"(4 字节) 覆盖，尾部 "?..." 后移
        size_t tail = strlen(pos + 7) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 7, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // /opus?q=... 转换为 /pcm?q=...（同样降低解码负荷）
    pos = strstr(url, "/opus?");
    if (pos) {
// "/opus" 占 6 字节，用 "/pcm"(4 字节) 覆盖，尾部 "?..." 后移
        size_t tail = strlen(pos + 5) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 5, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
// AI 可能返回不带前导 / 的 URL，同样处理
    pos = strstr(url, "stream?");
    if (pos && (pos == url || *(pos-1) != '/')) {
// "stream" 占 6 字节，用 "pcm"(3 字节) 覆盖，尾部 "?..." 后移
        size_t tail = strlen(pos + 6) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 6, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
    pos = strstr(url, "opus?");
    if (pos && (pos == url || *(pos-1) != '/')) {
// "opus" 占 4 字节，用 "pcm"(3 字节) 覆盖，尾部 "?..." 后移
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
// 共7路通道

// ============================================
// 四路主电机 (TB6612 / DRV8833)
// ============================================

// 通用 GPIO 初始化（支持直驱和 PWM 双模式）
static void motor_gpio_init(gpio_num_t in1, gpio_num_t in2) {
    gpio_config_t c = { .pin_bit_mask = (1ULL<<in1)|(1ULL<<in2),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}
// ============================================
/**
 * @brief 电机构造函数 - GPIO 直驱模式（非 PWM）
 *
 * 通过 GPIO 高低电平直接控制电机正反转。
 *
 * @param in1 GPIO 引脚号（正转引脚）
 * @param in2 GPIO 引脚号（反转引脚）
 */
Motor::Motor(gpio_num_t in1, gpio_num_t in2)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(false), max_duty_(0) { motor_gpio_init(in1, in2); Stop(); }
// ============================================
/**
 * @brief 电机构造函数 - PWM 模式（共享定时器，10-bit/1kHz）
 *
 * 使用 LEDC_MOTOR 共享定时器，10-bit 分辨率 (0~1023)，频率 1kHz。
 * 适合 TB6612 四路主电机驱动。
 *
 * @param in1 GPIO 引脚号（正转通道）
 * @param in2 GPIO 引脚号（反转通道）
 * @param ch1 LEDC 通道号（正转）
 * @param ch2 LEDC 通道号（反转）
 * @param sm LEDC 速度模式
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
 * @brief 电机构造函数 - PWM 模式（自定义定时器，8-bit/10kHz）
 *
 * 使用独立的自定义定时器，8-bit 分辨率 (0~255)，频率 10kHz。
 * 适合水车/鸟跳电机等需要独立频率控制的场景。
 *
 * @param in1 GPIO 引脚号（正转通道）
 * @param in2 GPIO 引脚号（反转通道）
 * @param ch1 LEDC 通道号（正转）
 * @param ch2 LEDC 通道号（反转）
 * @param timer LEDC 定时器编号
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
}
/**
 * @brief 设置电机速度
 *
 * @param speed -100（全速反转）到 100（全速正转），0=停止
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
 * @brief 停止电机（制动）
 */
void Motor::Stop() {
    if (use_pwm_) { ledc_set_duty(speed_mode_, ledc_channel_, 0); ledc_set_duty(speed_mode_, ledc_channel2_, 0);
        ledc_update_duty(speed_mode_, ledc_channel_); ledc_update_duty(speed_mode_, ledc_channel2_); }
    else { gpio_set_level(in1_pin_, 0); gpio_set_level(in2_pin_, 0); }
}
/**
 * @brief 电机正转
 *
 * @param speed 速度百分比（0~100），默认 100
 */
void Motor::Forward(int speed) { SetSpeed(speed > 0 ? speed : 100); }
/**
 * @brief 电机反转
 *
 * @param speed 速度百分比（0~100），默认 100
 */
void Motor::Reverse(int speed) { SetSpeed(speed > 0 ? -speed : -100); }

// ============================================
// ============================================
// ============================================
Servo::Servo(gpio_num_t pin, ledc_channel_t channel, ledc_mode_t speed_mode)
    : pin_(pin), angle_(90), ledc_channel_(channel), speed_mode_(speed_mode) {

// 50Hz PWM，14-bit 分辨率（16384 级精度）
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
 * @brief 设置舵机角度
 *
 * @param angle 目标角度（0°~180°），自动钳位到 [0, 180]
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


// config.h 配置: MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP



// ============================================

// ============================================




// ============================================

/**
 * @brief Mp3Player::~Mp3Player
 */
Mp3Player::~Mp3Player() {
    stop_requested_ = true;

    for (int i = 0; i < 100 && is_playing_; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  // 总计5秒超时
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

/**
 * @brief 初始化 MP3 播放器
 *
 * @param assets 资源管理器指针，用于读取 assets 分区文件
 */
void Mp3Player::Init(Assets* assets) {
    assets_ = assets;
    ESP_LOGI(TAG, "Mp3Player initialized (async task-based software decode)");
}




/**
 * @brief 更新 Ducking 状态（AI 说话时自动降低音乐音量）
 *
 * 渐变时间 400ms，最终降到 20% 音量。
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
 * @brief 加载狗叫 PCM 数据到播放缓冲区
 *
 * 等待上一段狗叫播放完毕后，将新的 PCM 数据拷贝到 bark_pcm_，
 * 由混音器在后续 tick 中叠加到后台音频上。
 *
 * @param pcm PCM 数据指针（16-bit 单声道）
 * @param num_samples 采样数
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
 * @brief 对 PCM 数据应用 Ducking 增益衰减
 *
 * AI 说话时将音乐音量降低到 ducking_gain_（默认 0.20）。
 * 增益 ≥ 1.0 时不处理，避免浪费 CPU。
 *
 * @param pcm PCM 数据缓冲区（原地修改）
 * @param num_samples 采样数
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
 * @brief 解码单个 MP3 文件并播放
 *
 * 从 assets 读取 MP3，软件解码后通过 OutputRawPcm 输出到 I2S。
 *
 * @param index MP3 文件编号
 * @return 0=失败，1=成功
 */
int Mp3Player::DecodeSingleFile(int index) {
    if (!assets_) {
        ESP_LOGE(TAG, "Assets not initialized");
        return 0;
    }

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

// 从 assets 分区读取 MP3 文件数据
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

// 跳过 ID3v2 标签头（10 字节头 + sync-safe 长度）
    uint8_t* mp3_start = (uint8_t*)mp3_data;
    size_t mp3_data_size = mp3_size;
    if (mp3_size > 10 && memcmp(mp3_start, "ID3", 3) == 0) {
        uint32_t id3_size = ((mp3_start[6] & 0x7F) << 21) | ((mp3_start[7] & 0x7F) << 14)
                          | ((mp3_start[8] & 0x7F) << 7)  |  (mp3_start[9] & 0x7F);
        size_t skip = 10 + id3_size;
if (skip < mp3_size - 1024) { //
            mp3_start += skip;
            mp3_data_size -= skip;
            ESP_LOGI(TAG, "Skipped ID3v2 tag: %lu bytes", (unsigned long)skip);
        }
    }

// MP3 软件解码处理
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return 0;
    }


    esp_audio_dec_info_t dec_info = {};
    bool info_ready = false;
    int sample_rate = 22050;
    int channels = 1;

// 设置 input_ptr 变量
    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 50;

    while (remaining > 0 && !stop_requested_) {
// 内存拷贝/移动
        size_t in_len = (remaining < kInputBufSize) ? remaining : kInputBufSize;
        memcpy(input_buf_, input_ptr, in_len);

        esp_audio_dec_in_raw_t raw;
        raw.buffer = input_buf_;
        raw.len = in_len;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

// 变量赋值
        esp_audio_dec_out_frame_t frame;
        frame.buffer = output_buf_;
        frame.len = kOutputBufSize;
        frame.needed_size = 0;
        frame.decoded_size = 0;

  // 用 esp_mp3_dec 软件解码 MP3 帧
        ret = esp_mp3_dec_decode(mp3_dec_handle_, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK) {
// 首次调用 esp_mp3_dec_decode 获取解码信息 dec_info
            if (!info_ready && frame.decoded_size > 0) {
                sample_rate = dec_info.sample_rate ? dec_info.sample_rate : 22050;
                channels = dec_info.channel ? dec_info.channel : 1;
                ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %.1f kbps",
                         sample_rate, channels, dec_info.bitrate / 1000.0f);
                info_ready = true;
            }

// PCM 原始音频数据
            if (frame.decoded_size > 0 && !stop_requested_) {
                int16_t* pcm_data = (int16_t*)frame.buffer;
                size_t num_samples = frame.decoded_size / sizeof(int16_t);

                auto& app = Application::GetInstance();

 // ---- Ducking 混音：AI 开始说话时淡出音乐 ----
                // 状态机：0=空闲 1=淡出中 2=已降音 3=恢复中
                static int duck_state = 0;  // 0=idle, 1=fading, 2=ducked, 3=restoring
                static int64_t duck_transition_us = 0;
                bool ai_speaking = (app.GetDeviceState() == kDeviceStateSpeaking);

                if (disable_ducking_) {
// 条件判断
                    ducking_gain_ = 1.0f;
                } else if (ai_speaking && duck_state == 0) {
// AI 语音交互
                    duck_state = 1;
                    duck_transition_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "DecodeSingleFile: AI speaking, fading out");
                }

                if (duck_state == 1) {
// 400ms 内恢复至 100% 音量
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ <= 0.2f) {
                        ducking_gain_ = 0.2f;
                        duck_state = 2;
                    }
                    if (!ai_speaking) {
// AI 语音交互
                        duck_state = 3;
                        duck_transition_us = esp_timer_get_time();
                    }
                } else if (duck_state == 2) {
                    ducking_gain_ = 0.2f;
                    if (!ai_speaking) {
                        duck_state = 3;
                        duck_transition_us = esp_timer_get_time();
                    }
                } else if (duck_state == 3) {
// 400ms 内恢复至 100% 音量
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 0.2f + 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ >= 1.0f) {
                        ducking_gain_ = 1.0f;
                        duck_state = 0;
                        ESP_LOGI(TAG, "DecodeSingleFile: false trigger, restored");
                    }
                    if (ai_speaking) {
// AI 语音交互
                        duck_state = 1;
                        duck_transition_us = esp_timer_get_time();
                    }
                }

 // 应用增益 + 限幅
                float gain = ducking_gain_;
                for (size_t i = 0; i < num_samples; i++) {
                    int32_t s = (int32_t)(pcm_data[i] * gain);
                    if (s > 30000) s = 30000;
                    if (s < -30000) s = -30000;
                    pcm_data[i] = (int16_t)s;
                }

// 条件分支
                if (bark_active_) {
                    size_t to_mix = num_samples;
                    if (bark_offset_ + to_mix > bark_total_) {
                        to_mix = bark_total_ - bark_offset_;
                    }
                    for (size_t i = 0; i < to_mix; i++) {
                        int32_t mixed = (int32_t)pcm_data[i] + (int32_t)bark_pcm_[bark_offset_ + i];
                        if (mixed > 32767) mixed = 32767;
                        else if (mixed < -32768) mixed = -32768;
                        pcm_data[i] = (int16_t)mixed;
                    }
                    bark_offset_ += to_mix;
                    if (bark_offset_ >= bark_total_) {
                        bark_active_ = false;
                        ESP_LOGI(TAG, "DogBark: mix done (%u samples)", (unsigned)bark_total_);
                    }
                }

                app.GetAudioService().OutputRawPcm(pcm_data, num_samples, sample_rate);

// 通过 OutputRawPcm 推送到 I2S 输出
                int play_ms = (int)(num_samples * 1000 / sample_rate / (channels > 0 ? channels : 1));
                if (play_ms < 10) play_ms = 10;
                vTaskDelay(pdMS_TO_TICKS(play_ms));
            }

// 条件判断
            size_t consumed = raw.consumed;
            if (consumed == 0) {
                consumed = 1;  //  Tiny files advance byte-by-byte
                if (consumed > remaining) consumed = remaining;
            }
consecutive_errors = 0; //
            input_ptr += consumed;
            remaining -= consumed;

        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer too small, needed %lu", (unsigned long)frame.needed_size);
            break;
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            ESP_LOGE(TAG, "Unsupported MP3 format (stopping)");
            break;
        } else {

            consecutive_errors++;
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "Too many consecutive decode errors (%d), aborting", consecutive_errors);
                break;
            }

/**
 * @brief HTTP 下载并播放 MP3 文件
 *
 * 建立 socket 连接，流式下载，逐批解码播放。
 *
 * @param url 完整的 HTTP URL
 * @return 0=成功，<0=失败
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
  return 0; // 
}

/**
 * @brief 验证 MPEG 音频帧头合法性
 *
 * 检查帧同步字 0xFFE 及各标志位是否合法。
 * 用于流式下载中定位有效 MP3 帧边界。
 *
 * @param p 待检测数据指针
 * @return true=合法帧头 false=无效
 */
// MPEG 帧头结构：帧同步字 FF + 比特率/采样率/层信息
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
 * @brief HTTP 批量流式下载 MP3 异步任务
 *
 * 建立 HTTP 连接，流式下载 MP3 数据，逐帧解码。
 * 支持 HTTP 302 重定向，提供下载进度回调。
 *
 * @param arg 线程参数（包含 URL 和播放器指针）
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

// HTTP 客户端配置
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

  // MP3 软件解码处理
    void* dec_handle = nullptr;
    esp_audio_err_t dec_ret = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PlayUrl: decoder fail %d", dec_ret);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }

    self->is_playing_ = true;
    app.GetAudioService().SetOutputMuted(true);

// 解析 Content-Length 获取文件大小
    size_t batch_size = 256 * 1024;
    if (content_length > 0 && content_length < 8 * 1024 * 1024) {
batch_size = content_length; //
  if (batch_size > 4 * 1024 * 1024) batch_size = 4 * 1024 * 1024; // 4MB
        ESP_LOGI(TAG, "PlayUrl: batch=%dKB (content=%dKB)", (int)(batch_size/1024), content_length/1024);
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
// PSRAM 分配失败，回退到 256KB 内部 RAM
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

// 累加操作
    const int kOutRate = 24000;
                // 缓冲区预留，容纳最大 MP3 帧 (1152 立体声) 从 8000Hz 升采样到 24000Hz
// 4096 缓冲区大小
// 4096 缓冲区大小
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
 // ============================================
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

    // ---- 跳过 ID3v2 标签头，定位到第一帧 ----
    mp3_start = 0;
    if (batch_len > 10 && memcmp(buf, "ID3", 3) == 0) {
        uint32_t id3_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14)
                          | ((buf[8] & 0x7F) << 7)  |  (buf[9] & 0x7F);
        mp3_start = 10 + id3_size;
if (mp3_start > batch_len - 1024) mp3_start = 0; // /
        ESP_LOGI(TAG, "PlayUrl: ID3v2 %luB, MP3 @ %u", id3_size, (unsigned)mp3_start);
    }

    // ---- 扫描缓冲区，查找所有 MPEG 帧头 ----
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
    batch_seq = 1;

    while (1) {
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
 // ---- Ducking：AI 说话时淡出音乐 ----
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

                    int16_t* pcm = (int16_t*)frame.buffer;
                    size_t ns = frame.decoded_size / sizeof(int16_t);  // stereo sample count
                    size_t mono_ns = ns / 2;

 // 通过 OutputRawPcm 推送到 I2S 输出
 // 通过 OutputRawPcm 推送到 I2S 输出
                    if (sample_rate != kOutRate && mono_ns > 0) {
                    // 第1步：立体声 → 单声道（左/右平均值）
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }
// 2 倍线性插值重采样到 kOutRate
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
// Ducking：AI 说话时自动降低音乐音量
                        float g = self->ducking_gain_;
                        for (size_t i = 0; i < out_ns; i++) {
                            int32_t s = (int32_t)(resample_buf2[i] * g);
                            if (s > 30000) s = 30000;
                            if (s < -30000) s = -30000;
                            resample_buf2[i] = (int16_t)s;
                        }
                        app.GetAudioService().OutputRawPcm(resample_buf2, out_ns, kOutRate);
                    } else {
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }
// Ducking：AI 说话时自动降低音乐音量
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
// frame_size 和 raw.consumed 用于跟踪解码进度
                size_t c = dec_info.frame_size;
if (c == 0) c = raw.consumed; //
                if (c == 0) c = 1;
                if (c >= rem) { rem = 0; } else { ptr += c; rem -= c; }
            } else if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (raw.consumed > 0 && raw.consumed < rem) { ptr += raw.consumed; rem -= raw.consumed; }
                else if (raw.consumed == 0) {
// MPEG 音频帧
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 < rem) { ptr += scan; rem -= scan; }
else { rem = 0; break; } // carry
                }
                if (rem < 1024) break;
            } else {
                if (raw.consumed > 0) {
                    if (raw.consumed >= rem) { rem = 0; }
                    else { ptr += raw.consumed; rem -= raw.consumed; }
                } else {
// MPEG 音频帧
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

  // ---- 开始新一轮 HTTP 下载 ----
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
// carry=0，跳过 metadata 块
        if (carry == 0 && rem > 3 && !IsValidMpegHeader(ptr)) {
            size_t s = 1;
            while (s + 3 < rem && !IsValidMpegHeader(ptr + s)) s++;
            if (s + 3 < rem) {
                ESP_LOGD(TAG, "PlayUrl: skip %d bytes to next frame", (int)s);
                ptr += s; rem -= s;
            } else {
rem = 0; //
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
 * @brief 通过 HTTP 下载并播放原始 PCM 音频
 * @param url PCM 文件的 HTTP URL
 * @return 0=失败，>0=HTTP 状态码
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
 * @brief HTTP PCM 播放异步任务
 *
 * 从 URL 下载原始 PCM 数据，推送至后台音频环形缓冲区。
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


const size_t CHUNK = sizeof(self->output_buf_); //
    const int SAMPLE_RATE = 24000;
    int64_t t0 = esp_timer_get_time();
    int bytes_yielded = 0;

    while (self->is_playing_ && !self->stop_requested_) {
        int read = esp_http_client_read(client, (char*)self->output_buf_, CHUNK);
        if (read <= 0) break;

size_t samples = read / 2; // 16
        int16_t* pcm = (int16_t*)self->output_buf_;

 // ---- Ducking：AI 说话时淡出音乐 ----
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
// Ducking：AI 说话时自动降低音乐音量
        float g = self->ducking_gain_;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = (int32_t)(pcm[i] * g);
            if (s > 30000) s = 30000;
            if (s < -30000) s = -30000;
            pcm[i] = (int16_t)s;
        }

        app.GetAudioService().PushRawPcmToPlayback(pcm, samples, SAMPLE_RATE);

// 4 核 CPU，WiFi 带宽管理
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
 * @brief 通过 HTTP 下载并解码播放 OGG/Opus 音频
 * @param url Opus 文件的 HTTP URL
 * @return 0=失败，>0=HTTP 状态码
 */
int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        ESP_LOGW(TAG, "PlayOpus: music already playing, refusing (call stop_music first)");
        return -1;
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
stop_requested_ = false; // Stop()
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;

    // 清理上次会话残留的后台音频，防止启动噪声爆音
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

/**
 * @brief HTTP Opus 播放异步任务
 *
 * 从 URL 下载 OGG/Opus 流，OGG 解封装后 Opus 解码输出。
 */
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

 // URL 地址处理

 // URL 地址处理
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
    
// BSD Socket 连接
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
// IP 地址与 DNS 解析
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
    
// 检测设备是否空闲，通知 AI 状态变更
// 检测设备是否空闲，通知 AI 状态变更
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
 // === 串口回退 ===
        
 // === 串口回退 ===
        printf("\x01MUSIC_REQ\x02%s\x03\n", url);
        fflush(stdout);
        
// 停止 PlayOpus 播放，重置 fread 状态
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
// AFE 音频前端处理
                uint64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_refresh_ms > 8000) {  // every 8s
 // HACK：长时间下载时防止音频看门狗超时
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
if (app.GetDeviceState() == kDeviceStateConnecting) break; // stop
 // 条件判断
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
    
 // 格式化字符串
    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    send(sock, req, strlen(req), 0);
    
// HTTP 客户端配置
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

 // === 格式检测：/opus 走 OGG 解封装 + 主音频管线 ===
 // === /pcm 走原始字节推入后台环形缓冲 ===
    bool use_opus = (strstr(path, "/opus") != nullptr);
    
    if (use_opus) {
        // ============ Opus 路径：OGG 解封装 → 推入解码队列 ============
// AI 语音 Opus 音频流
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
        
// AEC 回声消除率 65%
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

// 3-5 / 5-15 秒 AI 交互时间窗口
// 3-5 / 5-15 秒 AI 交互时间窗口
// 3-5 / 5-15 秒 AI 交互时间窗口
// 3-5 / 5-15 秒 AI 交互时间窗口
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
        
// 任务延时等待
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
    
 // ============ PCM 路径（兼容）：原始 s16le 推入后台音频 ============
    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    int64_t t0 = esp_timer_get_time(), last_refresh = t0;
    size_t total_dl = 0;
    bool ai_speaking = false;

    app.GetAudioService().SetBackgroundAudioGain(0.001f);  // near-silent but keeps bg_audio_active_=true
    ESP_LOGI(TAG, "PlayOpus: downloading raw PCM...");

 // === 预缓冲阶段：先填充环形缓冲再启用输出 ===
    int64_t prebuf_start = esp_timer_get_time();
    while (self->is_playing_ && !self->stop_requested_) {
        int read = recv(sock, buf, CHUNK, 0);
        if (read <= 0) break;
        total_dl += read;
// s16le PCM 格式：2 字节=1 采样
        app.GetAudioService().PushBackgroundAudio(
            reinterpret_cast<int16_t*>(buf), read / sizeof(int16_t), 16000);

        size_t fill = app.GetAudioService().GetBgAudioFillLevel();
        if (fill >= 64000) {
 // 在启用输出前根据 AI 状态设置增益
 // （防止全音量音乐 + TTS 重叠噪声）
            auto state = app.GetDeviceState();
            // 仅 AI 说话时降音，监听状态保持全音量（2026-07-19 用户要求）
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGI(TAG, "PlayOpus: drain enabled (buffered %d samples in %d ms)",
                     (int)fill, (int)((esp_timer_get_time() - prebuf_start) / 1000));
            break;
        }
        if (esp_timer_get_time() - prebuf_start > 10000000) {
 // 即使超时也要设置增益，否则停留在 0.001
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

 // === 主下载 + 推送循环 ===
    int recv_errors = 0;
    while (self->is_playing_ && !self->stop_requested_) {
        auto state = app.GetDeviceState();
        // 监听时保持 100%，仅 AI 说话/连接时降至 50%
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

 // AI 说话时暂停 TCP 下载，释放 WiFi 带宽给
 // UDP 音频包（防止 WiFi 缓冲不足导致 TTS 卡顿）。
 // 仅在缓冲充足时暂停，确保能支撑完整 AI 回复。
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
 // 服务器优雅关闭连接
            ESP_LOGI(TAG, "PlayOpus: server closed connection");
            break;
        } else {
 // read < 0：超时或瞬时错误
            recv_errors++;
            if (recv_errors > 5) {
                ESP_LOGE(TAG, "PlayOpus: recv failed %d times, giving up (errno=%d)", recv_errors, errno);
                break;
            }
 // 等待 1 秒后重试，WiFi 可能在 AI 对话后重连
            ESP_LOGW(TAG, "PlayOpus: recv error %d/%d (errno=%d), retrying...", recv_errors, 5, errno);
            for (int w = 0; w < 10 && self->is_playing_ && !self->stop_requested_; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
 // 刷新省电模式，应对通道关闭时的变更
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            continue;
        }

        while (app.GetAudioService().GetBgAudioFillLevel() > 80000 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_refresh > 8000000) {
 // 诊断日志：每 8 秒记录缓冲区填充 + 下载速率
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
        // 直接清空，不等 ring buffer 排空
// 原来 while 等待排空会导致 MusicDanceTick 继续跑 ~5 秒（79K 样本 16kHz）
        app.GetAudioService().ClearBackgroundAudio();
    }
    close(sock);
    self->is_playing_ = false;
    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "PlayOpus: done %dms, dl=%dKB", (int)total_ms, (int)(total_dl / 1024));

// 空闲监听状态
// 空闲监听状态
// 空闲监听状态
// 空闲监听状态
    if (app.GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "PlayOpus: restarting wake word detection after music");
        app.GetAudioService().EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        app.GetAudioService().EnableWakeWordDetection(true);
        // Fix(2026-07-19)：音乐期间进入 idle 导致唤醒阈值未恢复
        // (IsBgAudioActive 守卫)，导致停留在 0.30。在此恢复敏感值 0.02。
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 播放闹钟铃声
 * @param volume 音量系数（1.0=全音量）
 */
void Mp3Player::PlayAlarmRing(float volume) {
    auto& app = Application::GetInstance();
    const int sample_rate = 16000;
    const int chunk_ms = 100;  // 100ms chunks for responsive stop
    const int total_ms = 2000;
    const int chunk_samples = sample_rate * chunk_ms / 1000;
    const int total_chunks = total_ms / chunk_ms;

    const int freq_a = 880;   // A5
    const int freq_b = 1100;  // C#6
    const int base_amplitude = 8000;
    const int beep_on_ms = 80;
    const int beep_off_ms = 80;
    const int cycle_ms = beep_on_ms + beep_off_ms;

// 100ms 超时检查 stop_requested_ 标志
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
* @brief
* FreeRTOS pending_track_ / pending_bell_hour_
* AIAI
 */
 // 循环迭代
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
 // 播放单首曲目 - 此处管理静音
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
 // 播放钟声，使用短 PCM 钟声（每个 ~0.5s）
            self->is_playing_ = true;
            int hour = self->pending_bell_hour_;
            self->pending_bell_hour_ = 0;
            Application::GetInstance().GetAudioService().SetOutputMuted(true);
            for (int i = 0; i < hour; i++) {
                if (self->stop_requested_) break;
                self->bell_player_.PlayBellSoundSync();
 // 每次敲击间隔 ~1 秒，模拟自然报时钟声
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
 * @brief 按文件夹和曲目编号播放
 *
 * folder=1 → 播放钟声 (PlayBell)；
 * folder=2 → 播放 MP3 音乐 (1~12)，
 * 夹 3~5 → 播放 assets 分区 WAV/PCM 文件。
 *
 * @param folder 文件夹编号 (1=钟声, 2=MP3音乐, 3-5=assets)
 * @param track 曲目编号 (1-based)
 */
void Mp3Player::PlayTrack(uint8_t folder, uint8_t track) {
    if (folder == 1) {
  // 条件判断
        PlayBell(track);
    } else if (folder == 2) {
  // 条件分支
        if (track < 1) track = 1;
        if (track > 12) track = 12;
        PlayIndex(track);
    }
}

/**
 * @brief 按索引值播放 MP3 音乐
 *
 * 自动计算 folder/track，调用 PlayTrack。
 * 索引 1~12 → 曲目 1~12。
 *
 * @param index 曲目索引 (1-based)
 */
void Mp3Player::PlayIndex(uint16_t index) {
    if (index < 1) index = 1;

    if (!assets_) {
        ESP_LOGE(TAG, "Mp3Player not initialized (call Init first)");
        return;
    }

// 停止电机转动
    Stop();

// stop_requested_ 标志：请求停止播放
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;

// 条件分支
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
 */
void Mp3Player::Stop() {
    stop_requested_ = true;
// 调用 PlayOpus 播放 OGG/Opus 音频
// 调用 PlayOpus 播放 OGG/Opus 音频
    pending_track_ = 0;
    pending_bell_hour_ = 0;

    if (mp3_dec_handle_) {
        esp_mp3_dec_reset(mp3_dec_handle_);
    }
    
// 等待播放队列排空
    Application::GetInstance().GetAudioService().ResetDecoder();
}

/**
 * @brief 暂停播放
 */
void Mp3Player::Pause() {
    stop_requested_ = true;
    is_playing_ = false;
}

void Mp3Player::ResetForNextPlay() {
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    is_playing_ = false;
    pending_track_ = 0;
    pending_bell_hour_ = 0;
}

/**
 * @brief 恢复播放
 */
void Mp3Player::Resume() {
    ESP_LOGW(TAG, "Resume not supported, call PlayIndex again");
}

/**
 * @brief 设置音量
 *
 * @param vol 音量值
 */
void Mp3Player::SetVolume(uint8_t vol) {
    ESP_LOGI(TAG, "Volume request: %d (handled by AudioService)", vol);
}

/**
 * @brief 音量加
 */
void Mp3Player::VolumeUp() { SetVolume(20); }
/**
 * @brief 音量减
 */
void Mp3Player::VolumeDown() { SetVolume(10); }
/**
 * @brief 下一曲
 */
void Mp3Player::Next() {}
/**
 * @brief 上一曲
 */
void Mp3Player::Prev() {}

/**
 * @brief 播放报时钟声
 *
 * @param hour 当前小时数（用于确定钟声次数）
 */
void Mp3Player::PlayBell(int hour) {
    if (hour < 1) hour = 1;
    if (hour > 12) hour = 12;

    if (!assets_) return;

// 停止电机转动
    Stop();

 // 设置循环重复次数
    pending_bell_hour_ = hour;

// 条件分支
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
 * @brief 播放背景音乐
 *
 * @param index 音乐文件索引
 */
void Mp3Player::PlayBgMusic(int index) {
    if (index < 1) index = 1;
    PlayIndex(index);
}

/**
 * @brief 解码 MP3 文件到内存缓冲区（不直接播放）
 *
 * @param index MP3 文件编号
 * @param[out] out_buf 输出缓冲区指针
 * @param[out] out_samples 输出采样数
 * @param[out] out_samplerate 输出采样率
 * @return 0=成功，非0=失败
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

// MP3 到 PCM 转换，约 2.754 倍数据膨胀
    size_t max_samples = mp3_size * 4;
    if (max_samples < 8192) max_samples = 8192;
    if (max_samples > 2 * 1024 * 1024) max_samples = 2 * 1024 * 1024;  // cap at 2M samples (4MB)
    int16_t* buf = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu samples in PSRAM", (unsigned long)max_samples);
        return -1;
    }

// MP3 软件解码处理
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

// 跳过 ID3v2 标签头（10 字节头 + sync-safe 长度）
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

// 内存拷贝/移动
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
 // 解码器已缓冲全部输入，无法继续，停止
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

// MP3 软件解码处理
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
// ============================================
// ============================================
LdrSensor::LdrSensor(gpio_num_t adc_pin, adc_unit_t unit, adc_channel_t chan, int threshold)
    : adc_pin_(adc_pin), adc_handle_(nullptr), adc_chan_(chan), threshold_(threshold) {

 // ---- ADC Oneshot 初始化 ----
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
 * @brief 读取光敏电阻 ADC 原始值
 *
 * @return ADC 原始读数（0~4095，12-bit）
 */
int LdrSensor::ReadRaw() {
    int raw = 0;
    if (adc_handle_) {
        adc_oneshot_read(adc_handle_, adc_chan_, &raw);
    }
return raw; // 0-4095 (12-bit), =, =
}

/**
 * @brief 检测当前环境是否偏暗
 * @return true=偏暗（低于阈值）
 */
bool LdrSensor::IsDark() { return ReadRaw() < threshold_; }
void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }


// ============================================
// ============================================
// ============================================
/**
 * @brief 异步播放布谷鸟叫声
 *
 * 在 FreeRTOS 任务中执行，不阻塞调用者。
 * 数据来自 cuckoo_wake_sound.h 预编译 PCM。
 */
void BellSoundPlayer::PlayCuckooSoundSync() {
// AI TTS 播放期间，Opus 下载限速至 30%
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_wake_sound,
        CUCKOO_WAKE_SOUND_NUM_SAMPLES,
        CUCKOO_WAKE_SOUND_SAMPLE_RATE
    );
}

/**
 * @brief 同步播放钟声（阻塞式）
 *
 * 从 bell_0013.h 读取预编译 PCM 数据，通过 OutputRawPcm 直接输出。
 */
void BellSoundPlayer::PlayBellSoundSync() {
// AI TTS 播放期间，Opus 下载限速至 30%
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_bell_sound,
        CUCKOO_BELL_SOUND_NUM_SAMPLES,
        CUCKOO_BELL_SOUND_SAMPLE_RATE
    );
}

void BellSoundPlayer::PlayCuckooSoundAsync() {
    xTaskCreate([](void* arg) {
        auto* self = static_cast<BellSoundPlayer*>(arg);
        self->PlayCuckooSoundSync();
        vTaskDelete(NULL);
    }, "cuckoo_async", 2048, this, 5, NULL);
}

// ============================================
// ============================================
// ============================================
CuckooStateMachine::CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                                        Motor* violin_motor, Servo* violin, Servo* dog,
                                        Motor* water_bird, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog), // (B M2, GPIO18/45)
      water_bird_(water_bird),
      mp3_(mp3), bell_player_(bell_player), ldr_(ldr),
      motor_power_pin_(GPIO_NUM_NC),
      current_performance_(kPerformanceNone), current_phase_(kPhaseIdle),
      call_count_(0), total_calls_(0), is_running_(false),
      last_hour_(-1), last_half_hour_(-1), show_music_index_(0),
      dog_outro_running_(false), dog_intro_running_(false), kids_active_(false),
      current_hour_(12), current_min_(0), current_sec_(0), time_set_(false),
      is_dark_(false) {
    last_idle_exit_us_ = 0;
    prev_device_state_ = -1;
// P-MOSFET 电源控制：GPIO 低电平=通电，高电平=断电
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);
 // LED（GPIO直驱，5V）
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_A_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
MotorPowerOff(); // 磬100K 5V
  }

CuckooStateMachine::~CuckooStateMachine() {
    StopAll();
    MotorPowerOff();
}

/**
 * @brief 启用所有电机驱动电源
 * P-MOSFET 控制：GPIO 低电平=通电
 */
void CuckooStateMachine::MotorPowerOn() {
    gpio_set_level(motor_power_pin_, 0);
}

void CuckooStateMachine::MotorPowerOff() {
    gpio_set_level(motor_power_pin_, 1);
}

// ============================================
// 创建 PerformanceTask 或 ShowTask 异步演出任务
// ============================================

// 创建 PerformanceTask 或 ShowTask 异步演出任务
struct DoorOpenCtx {
    CuckooStateMachine* sm;
};

/**
 * @brief 异步开门任务
 *
 * 在 Core 1 后台执行：打开大门 → 小鸟跳跃（整点次数）
 * → 播放钟声 → 关门。
 *
 * @param arg 未使用的线程参数
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
 * @brief 播放狗叫声
 *
 * 若后台音频活跃→叠加混音，否则直接 PCM 输出。
 */
void CuckooStateMachine::PlayDogBark() {
// 加载 dog_bark.wav 音频资源（16kHz 单声道 s16le）
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

// PCM 数据通过 Mp3Player::DecodeSingleFile 解码
    const int16_t* pcm = (const int16_t*)(wav_data + 44);
    size_t pcm_bytes = wav_size - 44;
    size_t num_samples = pcm_bytes / sizeof(int16_t);

    // 将狗叫叠加到已有背景音频上（叠加混合，不是替换）
    // 若背景音频未激活则回退到 OutputRawPcm（如 DogShow）
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
 * @brief 舞蹈开场：开门 + 小鸟跳跃 + 狗出场
 *
 * 执行顺序：打开大门 → 小鸟跳跃 (x2)
 * → 小狗探出摇尾 → 狗叫。
 */
void CuckooStateMachine::RunDanceIntro() {
 // 重新使能电机电源，防止 MusicDogOutro 中途关闭
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open", 2048, ctx, 5, nullptr, 1);

// 先平滑回到 30，再归位
    if (dog_servo_) {
        dog_servo_->Sweep(180, 20, (180 - 20) * 15);
    }
 // 10. 小狗退回家中
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }
    PlayDogBark();
// 先平滑回到 30，再归位
    if (dog_servo_) {
        dog_servo_->Sweep(20, 0, 20 * 15);
    }
}

/**
 * @brief 舞蹈循环主体
 *
 * 等待背景音乐激活后，循环执行舞蹈电机、小提琴舵机、狗尾舵机等动作，
 * 直到音乐结束或被停止。
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

    // 背景音乐可能还在淡入，等待最多 5 秒
    // 待其激活，否则舞蹈循环下方直接退出。
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

// M1 舞蹈电机：低频模式 / 参数范围 1~3，>3 停止
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
     m1_rand_time = 1000 + (esp_random() % 2001); // 1~3
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
     m1_rand_time = 1000 + (esp_random() % 2001); // 1~3
                }
                break;
        }

// 9 次/秒节奏
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

// 先平滑回到 30，再归位
        if (dog_servo_) {
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

// LED 闪烁 6 次，周期约 300ms
        led_toggle++;
        if (led_toggle >= 6) {
            led_toggle = 0;
            led_state = !led_state;
            gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
        }

// 条件判断
        next_frame_us += 50000;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
        }
    }
}

/**
 * @brief 舞蹈终场：停车 + 小狗归位 + 关门
 *
 * 停止所有电机/舵机，小狗后退归位，
 * 关闭大门，停止水车 + 关闭 LED。
 */
void CuckooStateMachine::RunDanceFinale() {
 // ====== 演出开始：LED 亮 + 水车开始 ======
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);

// 约 279 帧/秒
    if (m1_fwd_time_ > 0 || m1_rev_time_ > 0) {
        long net = ((long)(m1_fwd_time_ - m1_rev_time_) * 279) / 1000;  // 旋转角度：+正转,-反转
        int rem = (int)(net % 360); if (rem < 0) rem = -rem;  // 取绝对值
        int ms; bool go_fwd;
        if (rem <= 180) {
            ms = rem * 1000 / 279;
            go_fwd = (net < 0);  // net为负=反转太多，补正转
        } else {
            ms = (360 - rem) * 1000 / 279;
            go_fwd = (net >= 0);  // net为正=正转太多，补反转绕一圈
        }
// 反转<正转时，补偿1.3
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

 // 10. 小狗退回家中
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(920));
        m3_->Stop();
    }
// 先平滑回到 30，再归位
    if (dog_servo_) {
        int dog_cur = dog_state_.angle;
        dog_servo_->Sweep(dog_cur, 180, (180 - dog_cur) * 15);
        dog_state_.angle = 180;
    }
 // 重新使能电机电源，防止 MusicDogOutro 中途关闭
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
}

// ============================================
// ============================================
// ============================================

static const char* kAlarmNvsNamespace = "cuckoo_alarm";
static const char* kAlarmNvsKey = "alarms";

/**
 * @brief 将闹钟配置写入 NVS（断电保留）
 *
 * 存储内容：闹钟小时/分钟、使能状态、每天重复标志。
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
 * @brief 从 NVS 加载闹钟配置（断电保留）
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
// 条件判断
    int count = 0;
    for (int i = 0; i < kMaxAlarms; i++) {
        if (alarms_[i].enabled) count++;
    }
    alarm_count_ = count;
    ESP_LOGI(TAG, "Alarms loaded from NVS (%d active)", count);
}


/**
 * @brief 启动报时/演出异步流程
 * @param type 演出类型（整点/半点/手动/背景音乐）
 * @param hour 当前小时（用于决定布谷鸟叫声次数）
 */
void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();

// 条件分支
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
if ((now_us - last_idle_exit_us_) < 5000000) { // 5
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }

// AI TTS 播放期间，Opus 下载限速至 30%
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
vTaskDelay(pdMS_TO_TICKS(200)); // idle
    }
// AI 语音交互
    app.GetAudioService().EnableVoiceProcessing(false);
// 重新使能语音处理
    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
total_calls_ = hour; //
call_count_ = 3; // 3
            break;
        case kPerformanceHalf:
            total_calls_ = 0;
call_count_ = 3; // 3
            break;
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
        default: is_running_ = false; return;
    }
    // 在 Core 1 上创建报时异步任务
    auto ret = xTaskCreatePinnedToCore(
        PerformanceTask,
        "cuckoo_perf",
        4096,
        this,
        5,
        nullptr,
        1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "StartPerformance: perf task create FAILED ret=%d", (int)ret);
        is_running_ = false;
        return;
    }
}


/**
 * @brief void CuckooStateMachine::PerformanceTask
 */
void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Performance: ENTER type=%d hour=%d min=%d", (int)sm->current_performance_, sm->current_hour_.load(), sm->current_min_.load());

// AI TTS 播放期间，Opus 下载限速至 30%
    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

// ====== 棨======
    if (sm->current_performance_ == kPerformanceHour) {

sm->OpenBirdDoor(); //
        for (int i = 0; i < sm->call_count_; i++) {
// 累加操作
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundAsync();
            }
vTaskDelay(pdMS_TO_TICKS(10)); //
 sm->BirdJumpPulse(); // 壨
            if (i < sm->call_count_ - 1) {
vTaskDelay(pdMS_TO_TICKS(1800)); // (~1.76s)
            }
        }
// 任务延时等待
        vTaskDelay(pdMS_TO_TICKS(2000));
sm->CloseBirdDoor(); //

 // 阶段 2：音乐+舞蹈（后台音频，与 start_show/LindaShow/GardenShow 同路径）
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            int16_t* bell_pcm = nullptr;
            size_t bell_samples = 0;
            int bell_sr = 0;
            int dec_ret = sm->mp3_->DecodeToBuffer(13, &bell_pcm, &bell_samples, &bell_sr);
            if (dec_ret == 0 && bell_pcm && bell_samples > 0) {
                ESP_LOGI(TAG, "Bell PCM loaded: %u samples @ %d Hz", (unsigned)bell_samples, bell_sr);
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    app.GetAudioService().OutputRawPcm(bell_pcm, bell_samples, bell_sr);
// 通过 OutputRawPcm 推送到 I2S 输出
                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                heap_caps_free(bell_pcm);
            } else {
 // PlayIndex：根据索引播放对应音频文件
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



app.GetAudioService().SetOutputMuted(false); // AI
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED); // IN1=HIGH

    // 第2阶段：音乐 + 舞蹈（背景音频，与 start_show/LindaShow/GardenShow 同路径）
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            sm->mp3_->SetDisableDucking(true);
            int song = sm->total_calls_;
            sm->show_music_index_ = song;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = (CuckooStateMachine*)arg;
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
            ESP_LOGI(TAG, "Perf: Phase2 bg_audio kids=%d intro_done=%d intro_run=%d dance_en=%d",
                     (int)sm->kids_active_.load(), (int)sm->dog_intro_done_.load(),
                     (int)sm->dog_intro_running_.load(), sm->music_dance_enabled_);

// 创建 ShowTask 异步演出任务
            unsigned long intro_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = static_cast<CuckooStateMachine*>(arg);
                s->RunDanceIntro();
                vTaskDelete(nullptr);
            }, "dance_intro", 2048, sm, 5, nullptr, 1);


            sm->RunDanceLoop();

            // 开场（开门+小狗，异步，~5 秒）必须在终场前完成
            // 小狗归位 / 关门，否则序列重叠。
            {
                unsigned long since_intro = xTaskGetTickCount() * portTICK_PERIOD_MS - intro_start_ms;
                if (since_intro < 6000) vTaskDelay(pdMS_TO_TICKS(6000 - since_intro));
            }
            sm->RunDanceFinale();
        }

    // 停止水车 + 关闭 LED
        if (sm->water_bird_) sm->water_bird_->Stop();
        gpio_set_level(LED_A_GPIO, 0);
        gpio_set_level(LED_B_GPIO, 0);

// ====== ++ ======
    } else if (sm->current_performance_ == kPerformanceHalf) {
sm->OpenBirdDoor(); //
        for (int i = 0; i < sm->call_count_; i++) {
// 累加操作
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundAsync();
            }
vTaskDelay(pdMS_TO_TICKS(10)); //
 sm->BirdJumpPulse(); // 壨
            if (i < sm->call_count_ - 1) {
vTaskDelay(pdMS_TO_TICKS(1800)); // (~1.76s)
            }
        }
// 任务延时等待
        vTaskDelay(pdMS_TO_TICKS(2000));
sm->CloseBirdDoor(); //
 // 音频服务调用
        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== 完成 ======
    if (sm->mp3_) sm->mp3_->Stop();


    sm->current_phase_ = kPhaseIdle;
    sm->MotorPowerOff();
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    ESP_LOGI(TAG, "Perf done: kids=%d dog_done=%d dog_run=%d dance_en=%d outro_done=%d", (int)sm->kids_active_.load(), (int)sm->dog_intro_done_.load(), (int)sm->dog_intro_running_.load(), sm->music_dance_enabled_, (int)sm->dog_outro_done_);
// 空闲监听状态
// 空闲监听状态
    if (app.GetDeviceState() == kDeviceStateIdle) {
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
/**
* @brief 鲢/
* - (22:00-6:00)
 * - AI
 * - (min==0): StartPerformance(kPerformanceHour)

/**
 * @brief 保存静音模式配置到 NVS
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
 * @brief 保存小人活跃状态到 NVS
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
 * @brief 从 NVS 加载小人活跃状态
 */
void CuckooStateMachine::LoadKidsActive() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "kids", &val) == ESP_OK) kids_active_ = (bool)val;
        nvs_close(nvs);
    }
}

// ============================================

// ============================================
/**
 * @brief 设置闹钟
 * @param hour 小时
 * @param minute 分钟
 * @param repeat_daily 是否每天重复
 */
void CuckooStateMachine::SetAlarm(int hour, int minute, bool repeat_daily) {
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (alarm_count_ >= kMaxAlarms) {
        ESP_LOGW(TAG, "Alarm list full (max %d)", kMaxAlarms);
        return;
    }
 // 检查重复并更新已有
    for (int i = 0; i < alarm_count_; i++) {
        if (alarms_[i].hour == hour && alarms_[i].minute == minute) {
            alarms_[i].enabled = true;
            alarms_[i].repeat_daily = repeat_daily;
            ESP_LOGI(TAG, "Alarm updated: %02d:%02d (repeat=%d)", hour, minute, repeat_daily);
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
    SaveAlarmsToNvs();
}

/**
 * @brief 获取所有闹钟的 JSON 列表
 * @return JSON 格式闹钟列表
 */
std::string CuckooStateMachine::GetAlarmsJson() {
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
 * @param index 闹钟索引（1-based）
 * @return true=删除成功
 */
bool CuckooStateMachine::DeleteAlarm(int index) {
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (index < 1 || index > alarm_count_) {
        ESP_LOGW(TAG, "Invalid alarm index: %d (have %d alarms)", index, alarm_count_.load());
        return false;
    }
 // 剩余闹钟向下移动
    for (int i = index - 1; i < alarm_count_ - 1; i++) {
        alarms_[i] = alarms_[i + 1];
    }
    alarm_count_--;
    ESP_LOGI(TAG, "Alarm %d deleted (remaining: %d)", index, alarm_count_.load());
    SaveAlarmsToNvs();
    return true;
}

/**
 * @brief 停止当前响铃的闹钟
 */
void CuckooStateMachine::StopAlarm() {
    alarm_stopped_ = true;
    alarm_ringing_ = false;
 // 单次闹钟：用户停止后禁用
    {
        std::lock_guard<std::mutex> lock(alarm_mutex_);
        for (int i = 0; i < alarm_count_; i++) {
            if (alarms_[i].enabled && !alarms_[i].repeat_daily
                && alarms_[i].hour == current_hour_
                && alarms_[i].minute == current_min_) {
                alarms_[i].enabled = false;
                ESP_LOGI(TAG, "One-shot alarm %02d:%02d disabled after stop",
                         alarms_[i].hour, alarms_[i].minute);
            }
        }
    }
 // 停止当前播放的音频
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

/**
 * @brief 检查并触发闹钟（每秒调用一次）
 */
void CuckooStateMachine::CheckAlarms(int hour, int minute, int sec) {
    if (alarm_ringing_) return;  // already ringing
    if (!time_set_) return;      // clock not set yet

    {
        std::lock_guard<std::mutex> lock(alarm_mutex_);
        for (int i = 0; i < alarm_count_; i++) {
            if (!alarms_[i].enabled) continue;
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
                break;  // only trigger one alarm at a time
            }
        }
    }
}

/**
 * @brief 闹钟响铃异步任务
 * 播放铃声 50 次（约 100 秒），前 10 次音量从 15% 渐增至 100%
 */
void CuckooStateMachine::AlarmTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Alarm task started");

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    while (sm->alarm_ringing_ && !sm->alarm_stopped_) {
 // 播放闹钟铃声 50 次（每次 2s = 总计 100s 响铃）
 // 第一轮：前 10 次（20s）音量从 15% 渐增至 100%
        for (int i = 0; i < 50; i++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            if (sm->mp3_) {
                float volume;
                if (i < 10) {
volume = 0.15f + 0.85f * (float)i / 9.0f; // 15% 100% (10)
                } else {
                    volume = 1.0f;
                }
                sm->mp3_->PlayAlarmRing(volume);
            }
        }

        if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;

 // 贪睡：等待 2 分钟，每秒检查 stopped_ 状态
        ESP_LOGI(TAG, "Alarm snoozing for 2 minutes...");
        for (int s = 0; s < 120; s++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 清理
    app.GetAudioService().SetOutputMuted(false);
    sm->alarm_ringing_ = false;
    sm->alarm_stopped_ = false;
    ESP_LOGI(TAG, "Alarm task ended");
    vTaskDelete(NULL);
}

// ============================================




// ============================================



/**
 * @brief void CuckooStateMachine::DogShow
 */
void CuckooStateMachine::DogShow() {
if (is_running_) return; //
// 累加操作
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);  // 任务完成后删除自身
    }, "dog_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief 小狗演出异步任务（Core 1）
 */
void CuckooStateMachine::DogShowTask() {
    auto& app = Application::GetInstance();

// 设置 is_running_ 变量
// 设置 is_running_ 变量

// 设置 is_running_ 变量
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");

    MotorPowerOn();

    // 1. 水车开始旋转
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 2. 打开大门
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 3. 狗尾探出：180° → 20°，用时 1200ms
    if (dog_servo_) {
        dog_servo_->Sweep(180, 20, 1200);
        dog_state_.angle = 20;
    }

    // 4. 小狗前进
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }

    // 5. 狗叫 #1
    PlayDogBarkDirect();

    // 6. 狗尾摆到 0°
    if (dog_servo_) {
        dog_servo_->Sweep(20, 0, 300);
        dog_state_.angle = 0;
    }

    // 7. 摇尾 10 秒：0° ↔ 60°
    unsigned long start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long end_time = start + 10000;
    int sweep_dir = 0;
    while (is_running_ && (xTaskGetTickCount() * portTICK_PERIOD_MS) < end_time) {
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

    // 8. 狗叫 #2
    PlayDogBarkDirect();

    // 9. 狗尾回到 20°
    if (dog_servo_) {
        dog_servo_->Sweep(0, 20, 300);
        dog_state_.angle = 20;
    }

    // 10. 小狗后退
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }

    // 11. 狗尾回到 180°
    if (dog_servo_) {
        dog_servo_->Sweep(20, 180, 1200);
        dog_state_.angle = 180;
    }

    // 12. 停止水车
    if (water_bird_) water_bird_->Stop();

    // 13. 关闭大门
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 清理
    ESP_LOGI(TAG, "Performance done: kids=%d dog_done=%d dog_running=%d dance_en=%d outro_done=%d",
             (int)kids_active_.load(), (int)dog_intro_done_.load(), (int)dog_intro_running_.load(),
             music_dance_enabled_, (int)dog_outro_done_);
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "DogShow: done");
}

/**
 * @brief 播放 WAV 音频文件
 * 查找 RIFF header + data chunk，直接输出 PCM
 * @param filename WAV 文件名
 */
void CuckooStateMachine::PlayWavAsset(const char* filename) {
    void* wav_ptr = nullptr;
    size_t wav_size = 0;
    if (!Assets::GetInstance().GetAssetData(filename, wav_ptr, wav_size)) {
        ESP_LOGW(TAG, "PlayWavAsset: %s not found", filename);
        return;
    }
    if (wav_size < 44) return;

    const uint8_t* wav_data = (const uint8_t*)wav_ptr;
    uint16_t channels = wav_data[22] | (wav_data[23] << 8);
    uint32_t sample_rate = wav_data[24] | (wav_data[25] << 8) | (wav_data[26] << 16) | (wav_data[27] << 24);
    uint16_t bits = wav_data[34] | (wav_data[35] << 8);
    if (bits != 16 || channels != 1) {
        ESP_LOGW(TAG, "PlayWavAsset: %s need mono s16", filename);
        return;
    }

    // 查找 data chunk（前面可能有 fmt、LIST 等块）
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
        // DogShow 路径：无背景音频，直接使用 OutputRawPcm
        app.GetAudioService().OutputRawPcm(pcm, num_samples, sample_rate);

/**
 * @brief 直接播放狗叫 PCM（不经过混音器）
 */
void CuckooStateMachine::PlayDogBarkDirect() {
    PlayWavAsset("dog_bark.wav");
}

bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;

 // 从 PSRAM 读取 MP3 数据
    int16_t* pcm = nullptr;
    size_t total_samples = 0;
    int src_sr = 0;
    if (mp3_->DecodeToBuffer(index, &pcm, &total_samples, &src_sr) < 0) {
        ESP_LOGE(TAG, "PlayShowMusicBg: decode failed for %04d.mp3", index);
        return false;
    }

    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 %u samples %dHz", index, (unsigned)total_samples, src_sr);

    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetBackgroundAudioGain(1.0f);

    const int DST_SR = 16000;
    const size_t CHUNK_SRC = 2048;  // push in small chunks
    size_t offset = 0;

    while (offset < total_samples && is_running_) {
        size_t chunk = (total_samples - offset > CHUNK_SRC) ? CHUNK_SRC : (total_samples - offset);

// 22050Hz 采样率，16000Hz 目标
        if (src_sr != DST_SR) {
            float ratio = (float)src_sr / DST_SR;
            std::vector<int16_t> dst(chunk * 2 / 3 + 2);  // ~30% smaller after resample
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

// 排空环形缓冲区
        while (audio.GetBgAudioFillLevel() > 64000 && is_running_)
            vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(pcm);
    // 推送完成：等待环形缓冲排空后清除背景音频标志
    // 确保演出循环在音乐真正结束时才退出。
    {
        int drain_wait = 0;
        while (is_running_ && audio.GetBgAudioFillLevel() > 0 && drain_wait < 200) {
            vTaskDelay(pdMS_TO_TICKS(50));
            drain_wait++;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        audio.ClearBackgroundAudio();
        ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 finished, bg audio cleared", index);
    }
    return true;
}

/**
 * @brief 创建琳达演出异步任务
 */
void CuckooStateMachine::StartLindaShow() {

if (is_running_) return; //
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->LindaShow();
        vTaskDelete(nullptr);
    }, "linda_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief 创建琳达演出异步任务
 */
void CuckooStateMachine::StartGardenShow() {

if (is_running_) return; //
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->GardenShow();
        vTaskDelete(nullptr);
    }, "garden_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief 琳达特别演出
 * 播放琳达主题音乐 + 舞蹈 + 小提琴 + 小狗 + 水车 + LED
 */
void CuckooStateMachine::LindaShow() {
    if (is_running_) { ESP_LOGW(TAG, "LindaShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

// 设置 is_running_ 变量

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");


 gpio_set_level(LED_A_GPIO, 1); // LED_A 
 gpio_set_level(LED_B_GPIO, 1); // LED_B 
 vTaskDelay(pdMS_TO_TICKS(500)); // 0.5

 // 2. 播放后台音频（TTS 通过 I2S 混音）
    if (mp3_) { mp3_->SetDisableDucking(true); int song = LINDA_SONG_BASE + (linda_song_counter_++ % LINDA_SONG_COUNT); ESP_LOGI(TAG, "LindaShow: playing %04d.mp3 (counter=%d)", song, (int)linda_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }

    // 等待背景音频开始排空
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

// 3. LED 闪烁 + 舞蹈电机启动
// 3. LED 闪烁 + 舞蹈电机启动
// 3. LED 闪烁 + 舞蹈电机启动
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long m1_timer = music_start_ms;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;
m1_fwd_time_ = 0; //
m1_rev_time_ = 0; //
    m1_stage_start_ = music_start_ms;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s safety timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

// 第 2、2、4 次 LED 闪烁
        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

// M1 舞蹈电机：低频模式 / 参数范围 1~3，>3 停止
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
// LED 第 2、4、6 次闪烁，周期 300ms
        if (!led_final) {
            led_toggle++;
            if (led_toggle >= 6) {
                led_toggle = 0;
                led_state = !led_state;
                gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
                gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }


 if (m1_) m1_->Stop(); // 赸

// 约 279 帧/秒
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
// 反转<正转时，补偿1.3
        if (ms > 0 && m1_fwd_time_ > m1_rev_time_) {
            ms = ms * 135 / 100;
        }
        if (ms > 0) {
            ESP_LOGI(TAG, "LindaShow: final: net=%lddeg rem=%ddeg, %s %dms", net, rem, go_fwd ? "fwd" : "rev", ms);
            if (go_fwd) m1_->Forward(100); else m1_->Reverse(100);
            vTaskDelay(pdMS_TO_TICKS(ms));
            m1_->Stop();
        }
    }

// 循环计数 0~4
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "LindaShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

 gpio_set_level(LED_A_GPIO, 0); // LED_A 
 gpio_set_level(LED_B_GPIO, 0); // LED_B 

    if (mp3_) mp3_->Stop();
 Application::GetInstance().GetAudioService().EnableBgAudioDrain(false); // bg audio
 Application::GetInstance().GetAudioService().EnableBgAudioDrain(false); // bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
if (mp3_) mp3_->SetDisableDucking(false); // duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "LindaShow: done");
}

/**
 * @brief 花园特别演出
 * 播放花园主题音乐 + 舞蹈 + 小提琴 + 小狗 + 水车 + LED
 */
void CuckooStateMachine::GardenShow() {
    if (is_running_) { ESP_LOGW(TAG, "GardenShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

// 设置 is_running_ 变量

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");


 gpio_set_level(LED_A_GPIO, 1); // LED_A 
 gpio_set_level(LED_B_GPIO, 1); // LED_B 
 vTaskDelay(pdMS_TO_TICKS(500)); // 0.5

 // 2. 播放后台音频（TTS 通过 I2S 混音）
    if (mp3_) { mp3_->SetDisableDucking(true); int song = GARDEN_SONG_BASE + (garden_song_counter_++ % GARDEN_SONG_COUNT); ESP_LOGI(TAG, "GardenShow: playing %04d.mp3 (counter=%d)", song, (int)garden_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }

    // 等待背景音频开始排空
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

// 3. LED 闪烁 + 舞蹈电机启动
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int violin_angle = SERVO_CENTER_ANGLE;
    int violin_dir = 0;  // 0: decreasing, 1: increasing
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s safety timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

// 条件分支
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
                gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
                gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

// 条件分支
    if (violin_servo_) {
violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000); // 90
    }

// 循环计数 0~4
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "GardenShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

 gpio_set_level(LED_A_GPIO, 0); // LED_A 
 gpio_set_level(LED_B_GPIO, 0); // LED_B 

    if (mp3_) mp3_->Stop();
 Application::GetInstance().GetAudioService().EnableBgAudioDrain(false); // bg audio
 Application::GetInstance().GetAudioService().EnableBgAudioDrain(false); // bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
if (mp3_) mp3_->SetDisableDucking(false); // duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "GardenShow: done");
}

/**
 * @brief 舞蹈表演（开门 + 电机舞蹈序列 + 关门）
 *
 * 综合演出入口，调用 RunDanceIntro + RunDanceLoop + RunDanceFinale。
 */
void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Dance start (Arduino-style)");

// M1 舞蹈电机：1-3 次正转 20ms，1-3 次反转 20ms，共 8 拍
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
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now;
                    m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }

 // （板型 B 小提琴电机 M2，via violin_motor_）
        if (violin_motor_) violin_motor_->Forward(VIOLIN_WARMUP_PERCENT);


        if (violin_servo_) violin_servo_->Sweep(45, 135, 1500);

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (m1_) m1_->Stop();
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);

/**
 * @brief 电机测试模式（MCP 调用）
 *
 * @param seconds 测试持续时间（1~60 秒）
 */
void CuckooStateMachine::MotorTest(int seconds) {
    if (is_running_) return;
    is_running_ = true;
    MotorPowerOn();
    ESP_LOGI(TAG, "MotorTest: M1 forward %d seconds...", seconds);
if (m1_) m1_->Forward(100); // GPIO
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    if (m1_) m1_->Stop();
    MotorPowerOff();
    is_running_ = false;
    ESP_LOGI(TAG, "MotorTest: done");
}


/**
 * @brief void CuckooStateMachine::StartShow
 */
void CuckooStateMachine::StartShow() {
// 演出/显示 AI 交互
    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }
// 任务延时等待
    if (is_running_) {
        StopAll();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

// 设置 is_running_ 变量
    is_running_ = true;
    current_performance_ = kPerformanceManual;

 // 在 Core 1 上运行
 // 在 Core 1 上运行
 // 在 Core 1 上运行
 // 在 Core 1 上运行
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
* @brief
 * AILED++()赸LED
 */
void CuckooStateMachine::StartShowTask() {
    auto& app0 = Application::GetInstance();
    // 立即启动 ShowTask（使用 PlayShowMusicBg 背景音频环形缓冲）
    // 音频服务通过 ducking 与 TTS 混合，无 I2S 冲突。
    ESP_LOGI(TAG, "Show: starting immediately (bg audio + ducking handles overlap)");

    app0.GetAudioService().SetWakeWordThreshold(0.99f);

MotorPowerOn();
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

    ShowTask(this);
}

/**
 * @brief 综合演出异步任务
 */
void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Show task started");

    auto& app = Application::GetInstance();


if (sm->mp3_) sm->mp3_->SetDisableDucking(true); //

    // ====== 演出开始：LED 亮 + 水车开始 ======
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED); // IN1=HIGH

// 状态判断
    int next = (esp_random() % 12) + 1;
    if (next == sm->show_music_index_ && next < 12) {
        next++;
    } else if (next == sm->show_music_index_) {
        next = 1;
    }
    sm->show_music_index_ = next;
    // 使用 PlayShowMusicBg（背景音频环形缓冲），同 LindaShow/GardenShow
    // 音频服务通过 ducking 混合音乐与 TTS，无 I2S 冲突。
    if (sm->mp3_) {
        sm->mp3_->SetDisableDucking(true);
        xTaskCreatePinnedToCore([](void* arg) {
            auto* s = (CuckooStateMachine*)arg;
            s->PlayShowMusicBg(s->show_music_index_);
            vTaskDelete(nullptr);
        }, "show_bg", 4096, sm, 4, nullptr, 1);
    }

    // 等待音乐开始播放（bg audio 缓冲就绪）
    int wait_start = 0;
    while (wait_start < 6 && sm->is_running_ && !Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_start++;
    }

    sm->violin_state_.fwd_count = 0;
    sm->violin_state_.rev_count = 0;
    ESP_LOGI(TAG, "DOG-MOTOR: m3_=%p", sm->m3_);

// 累加操作
    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);


    sm->RunDanceLoop();






if (sm->mp3_) sm->mp3_->SetDisableDucking(false); // duck

// 判断是否为设备空闲状态
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "Show: music ended, running dance finale");

    sm->RunDanceFinale();

    // 在终场完成后清理背景音频（之前提前清理导致状态间隙）
    app.GetAudioService().EnableBgAudioDrain(false);
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "Show: bg audio cleaned up, wake word threshold will be restored by clock task");
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;

    // ====== 停止水车 + 关闭 LED ======
    if (sm->water_bird_) sm->water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    sm->MotorPowerOff();

 // 导致时钟任务永远无法恢复 0.02 阈值，卡在 0.30（难以唤醒）。
    // Fix(2026-07-19)：演出运行时设备保持 idle，无状态切换，
    // 时钟任务永远无法恢复 0.02，停留在 0.30（难以唤醒）。
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    else
        app.GetAudioService().SetWakeWordThreshold(0.30f);

    ESP_LOGI(TAG, "Show task finished");
    vTaskDelete(NULL);
}

/**
 * @brief 播放指定索引的本地音乐（MCP 入口）
 *
 * 调用 Mp3Player::PlayIndex 播放 assets 分区中的 MP3 文件。
 *
 * @param index 曲目索引 (1-based)
 */
void CuckooStateMachine::PlayMusic(int index) {
    if (mp3_) mp3_->PlayBgMusic(index);
}

/**
 * @brief 停止所有演出活动（音乐+电机+舵机）
 */
void CuckooStateMachine::StopAll() {
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
 if (m1_) m1_->Stop(); // 赸
 if (m2_) m2_->Stop(); // 
if (m3_) m3_->Stop(); //
 if (m4_) m4_->Stop(); // 
if (violin_motor_) violin_motor_->Stop(); //
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
            mp3_->Stop();
        }
    }

// 中断语音播报 TTS
    auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        Application::GetInstance().Schedule([]() {
            auto s = Application::GetInstance().GetDeviceState();
            if (s == kDeviceStateSpeaking || s == kDeviceStateListening) {
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            }
            Application::GetInstance().GetAudioService().EnableVoiceProcessing(true);
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
            Application::GetInstance().GetAudioService().RefreshOutputTimestamp();
            Application::GetInstance().GetAudioService().RefreshInputTimestamp();
        });
    }
    ESP_LOGI(TAG, "All motors stopped");
    MotorPowerOff();
}

/**
 * @brief 停止当前播放的音乐
 */
void CuckooStateMachine::StopMusic() {
    if (mp3_) {
        mp3_->Stop();
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

/**
 * @brief 打开大门（M2 电机正转）
 */
void CuckooStateMachine::OpenDoor() {
    MotorPowerOn();
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
    MotorPowerOff();
}

/**
 * @brief 关闭大门（M2 电机反转）
 */
void CuckooStateMachine::CloseDoor() {
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
    MotorPowerOff();
}

/**
 * @brief 布谷鸟跳一次（500ms 上冲 + 400ms 回落）
 */
void CuckooStateMachine::BirdJumpOnce() {
    MotorPowerOn();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    MotorPowerOff();
}

void CuckooStateMachine::BirdJumpPulse() {
    MotorPowerOn();
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
}

/**
 * @brief 布谷鸟短跳（AI 说话时的快速鸟跳）
 */
void CuckooStateMachine::BirdJumpShort() {
 // 重新使能电机电源，防止 MusicDogOutro 中途关闭
 // 重新使能电机电源，防止 MusicDogOutro 中途关闭
 // 重新使能电机电源，防止 MusicDogOutro 中途关闭
    MotorPowerOn();
if (water_bird_) water_bird_->Stop(); // GPIO39
 gpio_set_level(MOTOR_WATER_BIRD_IN2, 1); // 磬
 int pulse = 50 + (esp_random() % 151); // 50-200ms
    vTaskDelay(pdMS_TO_TICKS(pulse));
gpio_set_level(MOTOR_WATER_BIRD_IN2, 0); // 磬
int cooldown = 300 + (esp_random() % 401); // 300-700ms
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

void CuckooStateMachine::MusicDanceTick() {
    // Mp3Player 跟踪播放状态（AI 说话 ducking 期间保持 true），
    // 而 IsBgAudioActive() 在音频服务清理缓冲时可能短暂下降。
    // 同时使用两者确保音乐舞蹈在 AI 对话期间不中断。
    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();
    // 跟踪 kids_active_ 状态切换，处理音乐中途变化
    static bool was_kids_active = true;
    bool kids_now = kids_active_.load();

    if (music_playing && !IsRunning()) {
        ESP_LOGI(TAG, "MusicDanceTick: mp=%d,dance_en=%d,kids=%d,intro_done=%d,intro_run=%d",
                 (int)music_playing, music_dance_enabled_, (int)kids_now,
                 (int)dog_intro_done_.load(), (int)dog_intro_running_.load());
        if (music_dance_enabled_ != 1) {
            music_dance_phase_ = 0;
            music_dance_enabled_ = 1;
            dog_outro_done_ = false;
            was_kids_active = kids_now;
        }

        // Dog comes out when music plays & kids active. NOT gated on
        // music_dance_enabled_ 的 init 块判断（可能被过期状态跳过）。
        if (kids_now && !dog_intro_running_ && !dog_intro_done_) {
            dog_intro_running_ = true;
            MusicDogIntro();
        }


        if (!was_kids_active && kids_now && !dog_intro_running_ && !dog_intro_done_) {
            was_kids_active = true;
            dog_outro_done_ = false;
            dog_intro_running_ = true;
            MusicDogIntro();
        }

        if (was_kids_active && !kids_now) {
            was_kids_active = false;
            if (m1_) m1_->Stop();
            if (violin_servo_) violin_servo_->SetAngle(90);
            if (!dog_outro_done_) {
                dog_outro_done_ = true;
                auto ret = xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 6, nullptr, 1);
                if (ret != pdPASS) ESP_LOGE(TAG, "MusicDanceTick: dog_outro create FAILED ret=%d", (int)ret);
            }
        }

        int phase = music_dance_phase_ % 8;

        // 等待狗出场动画完成后才接管舵机/电机
        if (kids_now && dog_intro_done_) {
            // 舞蹈电机：Phase 0-3 正转 20ms，Phase 4-7 反转 20/21ms
            if (phase <= 3 && m1_) {
                m1_->Forward();
                vTaskDelay(pdMS_TO_TICKS(20));
                m1_->Stop();
                m1_music_fwd_count_++;
            } else if (phase == 7 && m1_) {
                m1_->Reverse();
                vTaskDelay(pdMS_TO_TICKS(24));
                m1_->Stop();
                m1_music_rev_count_++;
            } else if (phase >= 4 && m1_) {
                m1_->Reverse();
                vTaskDelay(pdMS_TO_TICKS(20));
                m1_->Stop();
                m1_music_rev_count_++;
            }
            
            // 吉他 + 狗尾舵机：同步同相位节奏
            // Phase 0-3：前拍，Phase 4-7：后拍
            static int guitar_angle = 90;
            static int dog_angle = 40;
            bool forward_beat = (phase <= 3);
            int guitar_target = forward_beat ? 110 : 70;   // +/-20
            int dog_target = forward_beat ? 40 : 10;         // dog wags 10-40

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
        ESP_LOGI(TAG, "MusicDT-ELSE: mp=%d,dance_en=%d,kids=%d,intro_done=%d,intro_run=%d,outro_done=%d",
                 (int)music_playing, music_dance_enabled_, (int)kids_now,
                 (int)dog_intro_done_.load(), (int)dog_intro_running_.load(), (int)dog_outro_done_);
        was_kids_active = kids_now;
        if (music_dance_enabled_ == 1) {
            // M1 电机平衡：反转功率匹配正转
            if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
                int diff = m1_music_fwd_count_ - m1_music_rev_count_;
                ESP_LOGI(TAG, "MusicDance: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
                for (int i = 0; i < diff; i++) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(30));
                    m1_->Stop();
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            m1_music_fwd_count_ = 0;
            m1_music_rev_count_ = 0;
            if (m1_) m1_->Stop();
            if (violin_servo_) violin_servo_->SetAngle(90);
            // 音乐结束：狗回家，关门（仅在小人曾激活时）
            if (!dog_outro_done_) {
                dog_outro_done_ = true;
                auto ret = xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 6, nullptr, 1);
                if (ret != pdPASS) ESP_LOGE(TAG, "MusicDanceTick: dog_outro(end) create FAILED ret=%d", (int)ret);
            }
        }
        dog_intro_done_ = false;
        music_dance_enabled_ = 0;
    }
}

/**
 * @brief 音乐播放时小狗出场动画
 *
 * 狗尾舵机探出 (180° → 20°) → 小狗前进
 * → 摇尾。异步执行，由 MusicDanceTick 状态机触发。
 */
void CuckooStateMachine::MusicDogIntro() {
    ESP_LOGI(TAG, "MusicDogIntro: ENTER");
    dog_intro_running_ = true;
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    auto ret = xTaskCreatePinnedToCore(DoorOpenTask, "door_open_m", 2048, ctx, 6, nullptr, 1);
    if (ret != pdPASS) ESP_LOGE(TAG, "MusicDogIntro: door_open_m create FAILED ret=%d", (int)ret);
    if (dog_servo_) {
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
    // 重新使能电机电源，防止 MusicDogOutro 中途关闭
    MotorPowerOn();
    dog_intro_done_ = true;
    dog_intro_running_ = false;
    ESP_LOGI(TAG, "MusicDogIntro: done, handing over to MusicDanceTick");
}

/**
 * @brief 音乐结束时小狗归位动画
 *
 * 狗尾舵机归位 (cur° → 20°) → 小狗后退
 * → 狗尾归位 (20° → 180°) → 关门。
 */
void CuckooStateMachine::MusicDogOutro() {
    dog_outro_running_ = true;
    // 先平滑回到 30°，再归位
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
        dog_servo_->Sweep(20, 180, (180 - 30) * 15);
    }
    CloseDoor();
    MotorPowerOff();
    dog_intro_done_ = false;
    dog_outro_running_ = false;
}

/**
 * @brief 乐队小人出场
 *
 * 设置 kids_active_=true，若音乐播放中则触发狗出场动画。
 */
void CuckooStateMachine::KidsComeOut() {
    kids_active_ = true;
    SaveKidsActive();
    ESP_LOGI(TAG, "KidsComeOut: kids active, will dance with music");
    // Safety: if music is playing and dog isn't out yet, force-create dog_intro.
    // 绕过 MusicDanceTick 状态机，它可能因
    // Core 1 上优先级 6 任务之间的调度竞争而错过狗出场。
    // 用户显式要求小人出场，强制创建狗出场动画（忽略过期状态标志）。
    // Don't check dog_intro_done_ or dog_intro_running_ which can be corrupted by
    // MusicDanceTick 和 PerformanceTask 的竞争条件污染。
    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();
    if (music_playing && !IsRunning()) {
        dog_outro_done_ = false;
        music_dance_enabled_ = 1;
        dog_intro_done_ = false;
        dog_intro_running_ = true;
        MusicDogIntro();
    }
}

/**
 * @brief 小人休息状态（音乐停止、小人归位）
 *
 * 闭门 + 小狗归位（通过 MusicDogOutro），
 * 吉他手舵机保持 90°不动。
 */
void CuckooStateMachine::KidsRest() {
    kids_active_ = false;
    SaveKidsActive();
    // M1 电机平衡：反转功率匹配正转 before stopping
    if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
        int diff = m1_music_fwd_count_ - m1_music_rev_count_;
        ESP_LOGI(TAG, "KidsRest: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
        for (int i = 0; i < diff; i++) {
            m1_->Reverse();
            vTaskDelay(pdMS_TO_TICKS(30));
            m1_->Stop();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    m1_music_fwd_count_ = 0;
    m1_music_rev_count_ = 0;
    // 停止所有运动
    if (m1_) m1_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    if (dog_servo_) dog_servo_->SetAngle(dog_state_.angle);  // hold current position
    ESP_LOGI(TAG, "KidsRest: kids resting, no movement");
}

/**
 * @brief 打开小鸟门（M4 电机正转）
 */
void CuckooStateMachine::OpenBirdDoor() {
    MotorPowerOn();
    if (m4_) {
        m4_->Forward(BIRD_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
    MotorPowerOff();
}

/**
 * @brief 关闭小鸟门（M4 电机反转）
 */
void CuckooStateMachine::CloseBirdDoor() {
    MotorPowerOn();
    if (m4_) {
        m4_->Reverse(BIRD_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
    MotorPowerOff();
}

/**
 * @brief 播放布谷鸟叫声（通过 BellSoundPlayer）
 */
void CuckooStateMachine::PlayCuckooSound() {
    MotorPowerOn();
    if (bell_player_) {
        bell_player_->PlayCuckooSoundSync();
    }
    MotorPowerOff();
}

/**
 * @brief 设置舵机角度
 * @param servo_id 1=小提琴手臂，2=狗尾
 * @param angle 角度（0°~180°）
 */
void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
MotorPowerOn(); //
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

/**
* @brief
 * @param motor_id 1=M1赸, 2=, 3=, 4=
* @param speed -100~100 (=)
 */
void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break; // (B M2, GPIO18/45)
        case 3: if (m3_) m3_->SetSpeed(speed); break;
        case 4: if (m4_) m4_->SetSpeed(speed); break;
        default: break;
    }
    if (speed == 0) MotorPowerOff();
}

void CuckooStateMachine::SetBirdDoorSpeed(int speed) {
    if (speed != 0) MotorPowerOn();
    if (m4_) m4_->SetSpeed(speed);
    if (speed == 0) MotorPowerOff();
}

/**
* @brief
* @param url_or_path URL "/pcm?q="
* @return 0=, <0=
* - AICPU
* - URL
* - http
* -
 */
std::string CuckooStateMachine::CantoneseLookup(const char* word) {
    if (music_proxy_host_.empty()) {
        ESP_LOGW(TAG, "CantoneseLookup: proxy not configured");
        return "";
    }

    // 手动对单词做 URL 编码（esp_http_client 不支持中文 URL）
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

    // 读取 HTTP 响应体
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
 * @brief 检查是否为多歌手歌曲（向代理发送 HTTP 查询）
 *
 * 向 QQ 音乐代理发送请求，通过第一个字节判断返回 JSON 还是音频流，
 * 若为 JSON 则返回给 AI 解析多歌手信息。
 *
 * @param url_or_path 歌曲 URL 或路径
 * @return JSON 字符串（含 multi_artist 字段），或空字符串（音频流）
 */
std::string CuckooStateMachine::CheckMultiArtist(const char* url_or_path) {
    if (music_proxy_host_.empty()) return "";

    // URL编码
    char encoded_path[512];
    // 第0步：统一路径格式，把 /opus 或 /stream 都转成 /pcm
    char conv_url[512];
    strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
    conv_url[sizeof(conv_url) - 1] = '\0';
    ConvertToPcmUrl(conv_url, sizeof(conv_url));
    
    int ep = 0;
    for (const char* p = conv_url; *p && ep < (int)sizeof(encoded_path) - 4; p++) {
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

    // 第2步：构造完整URL，直接用原始路径不转换
    // /pcm 本身就能返回JSON或PCM，无需改路径
    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
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

    // 第3步：没有 Content-Length = 流式传输 = 音频（不是JSON），跳过
    if (content_len <= 0) {
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        return "";
    }

    // 第4步：读第一个字节 —— JSON 以 { 开头，音频是二进制乱码
    char first_byte = 0;
    int peek = esp_http_client_read(cli, &first_byte, 1);
    if (peek <= 0 || first_byte != '{') {
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        return "";
    }

    // 第5步：读完整个 JSON 响应体
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

    // 第6步：确认包含 multi_artist 字段后，返回JSON给AI
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
 * @brief 播放在线音乐（MCP 入口）
 *
 * 自动识别 URL 格式 (Opus/PCM)，停止旧任务后启动新播放。
 * 若 URL 含中文，手动 URL Encode 后再调用 Mp3Player。
 *
 * @param url_or_path QQ 音乐代理 URL 或搜索词
 * @return 0=成功，-1=失败
 */
int CuckooStateMachine::PlayOnlineMusic(const char* url_or_path) {
    if (!mp3_) return -1;
    
// AI TTS 播放期间，Opus 下载限速至 30%
    auto& app = Application::GetInstance();
    auto ai_state = app.GetDeviceState();
    if (ai_state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "PlayOnlineMusic: AI speaking, music will overlap (ducked)");
    }
    
    // 如果已在播放，干净停止旧任务后启动新任务。
    
    // 从 URL 路径自动检测格式
// URL 构造：http:// 前缀
    if (strncmp(url_or_path, "http", 4) == 0) {
        char conv_url[1280];
        strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
        conv_url[sizeof(conv_url) - 1] = '\0';
// URL 编码：空格转 %20
        for (char* p = conv_url; *p; p++) {
            if (*p == ' ') {
// 双倍/第二项
                size_t tail_len = strlen(p + 1);
                if ((size_t)(p + 3 + tail_len - conv_url) >= sizeof(conv_url)) break;
                memmove(p + 3, p + 1, tail_len + 1);
                p[0] = '%'; p[1] = '2'; p[2] = '0';
            }
        }
        ConvertToPcmUrl(conv_url, sizeof(conv_url));

        if (mp3_->IsPlaying()) {
            ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
            mp3_->Stop();
            vTaskDelay(pdMS_TO_TICKS(200));
// PlayOpusTask 完成后设置 is_playing_ = false
            int wait = 0;
            while (mp3_->IsPlaying() && wait < 100) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
// 缓冲区管理
            Application::GetInstance().GetAudioService().ClearBackgroundAudio();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return mp3_->PlayOpus(conv_url);
    }
    

    if (music_proxy_host_.empty()) {
        ESP_LOGE(TAG, "Music proxy not configured. Check DEFAULT_MUSIC_PROXY_HOST in config.h.");
        return -10;
    }
    
 // URL 编码非 ASCII 字符（中文等），esp_http_client 不支持原始中文 URL
    char encoded_path[1024];
    const char* src = url_or_path;
    char* dst = encoded_path;
    const char* end = encoded_path + sizeof(encoded_path) - 1;
    
    while (*src && dst < end) {
        unsigned char c = (unsigned char)*src;
        if (c == ' ') {
// HTTP 请求行构造
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
  // ASCII 范围外的字符编码为 %XX 格式
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
    
    char full_url[1280];  // http:// + host + :port + encoded_path
 // 确保路径以 / 开头
    if (encoded_path[0] != '/') {
        snprintf(full_url, sizeof(full_url), "http://%s:%d/%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    } else {
        snprintf(full_url, sizeof(full_url), "http://%s:%d%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    }
    ConvertToPcmUrl(full_url, sizeof(full_url));

    if (mp3_->IsPlaying()) {
        ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
        mp3_->Stop();
        vTaskDelay(pdMS_TO_TICKS(200));
        int wait = 0;
        while (mp3_->IsPlaying() && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait++;
        }
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return mp3_->PlayOpus(full_url);
}

/**
* @brief
 * @param host IP
* @param port
 */
void CuckooStateMachine::SetMusicProxy(const char* host, int port) {
    music_proxy_host_ = host;
    music_proxy_port_ = port;
    ESP_LOGI(TAG, "Music proxy set to %s:%d", host, port);
}

// ============================================
// ============================================
// ============================================
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

void CuckooTools::RegisterAll() {
    auto& mcp = McpServer::GetInstance();

 // === 时间 / 报时 ===
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
    // === 粤语查询 (2026-07-19) ===
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

 // === 音乐 / 演出 ===
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
        "�ۺϱ��ݣ��赸+С��+ˮ��+����һ������ '����' '��Ŀ' '��������' '����' '�ݳ�' ���ۺϱ�������"
        "ע�⣺����û�ֻ�ᵽĳ����ɫ��԰��/�մ�/С��������Ҫ�ô˹��ߣ����ö�Ӧ�Ľ�ɫ���ߡ�ʶ���ı������ӽ���ɫ��ʱ����'Ӧ��''�յ�''�ִ�'���մ'ԭַ'��԰�ӣ���Ҳ�ö�Ӧ��ɫ���ߣ���Ҫ�ô˹��ߡ�",
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
        "Stop music playback. Call ONLY when user explicitly asks to stop the music (ͣ��/��ͣ����/��Ҫ����/�ص�). Do NOT call this for performance or alarm - use cuckoo.stop_all for those.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopMusic();
            return std::string("{\"status\": \"music_stopped\"}");
        });

    // === 小朋友们控制 ===
    mcp.AddTool("cuckoo.kids_come_out",
        "让小朋友们出来一起欣赏音乐：小狗舵机、舞蹈电机、吉他舵机随音乐节奏摆动。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsComeOut();
            return std::string("{\"status\": \"kids_come_out\"}");
        });

    mcp.AddTool("cuckoo.kids_rest",
        "让小朋友们回去休息：停止所有动作（小狗舵机、舞蹈电机、吉他舵机不动），只保留音乐播放。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsRest();
            return std::string("{\"status\": \"kids_rest\"}");
        });

 // === 硬件控制 ===
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




    // === 闹钟 ===
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
        "Stop a ringing ALARM only. For ������/ͣ����. NOT for stopping music/performance - use cuckoo.stop_all for that.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === 静音模式 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("mode", kPropertyTypeInteger, 0, 3));
        pl.AddProperty(Property("start_hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("end_hour", kPropertyTypeInteger, 0, 23));
        mcp.AddTool("cuckoo.set_quiet_mode",
            "Set chime quiet mode. 0=ȫ�쾲��(������ʱ), 1=ȫ�챨ʱ, 2=��ھ���(LDR����), 3=ָ��ʱ��ξ���(start_hour~end_hour����). "
            "Mode 2 uses light sensor only (no time limit). Mode 3 defaults to 22-6.",
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
        "Get quiet mode (0=silent-24h, 1=chime-24h, 2=dark-silent-LDR-only, 3=time-window-22-6). "
        "Mode 2: no time limit, only LDR brightness matters. Mode 3: uses start_hour/end_hour only.",
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
                m == 2 ? "��ھ���(LDR)" : "ʱ��ξ���");
            return std::string(json);
        });

    // === 状态查询 ===
    mcp.AddTool("cuckoo.get_status",
        "查询设备状态：running（是否正在表演），music_playing（音乐是否正在播放）。\n"
        "【强制规则】回复用户任何关于演出/表演/音乐/歌舞的问题前，必须先调用本工具。不得凭记忆或上下文猜测状态。\n"
        "如果 running 是 false 但用户提到演出，直接说演出已经结束，不要说还在进行。",
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

 // === 在线音乐 ===
 // 音乐代理在启动时自动配置（config.h 里的 DEFAULT_MUSIC_PROXY_HOST/PORT）。
 // set_music_proxy 是空操作占位工具，永远瞬间成功，不做TCP连接检查。
 // 这个工具只是为了满足 AI 在调用 play_url 之前先调 set_music_proxy 的习惯。

    {
        PropertyList pl;
        pl.AddProperty(Property("url", kPropertyTypeString));
        mcp.AddTool("cuckoo.play_url",
            "播放在线音乐。用户说出歌名后立即调用本工具，不要先问用户问题。\n""参数url格式：'/pcm?q=歌名'（中文需要URL编码，空格用%%20）。\n""【最重要规则】只传歌名本身，绝不要自作主张加歌手名！系统会自动检测多版本并询问用户。\n""如果正在放歌，用户要换歌，直接传新歌名即可，系统自动停旧播新。\n""不要问用户'要不要换'——直接调用工具。\n""不要光说'我来放歌'——必须实际调用play_url。\n""示例：用户说'放酒干倘卖无'→调用play_url(\'/pcm?q=%E9%85%92%E5%B9%B2%E5%80%98%E5%8D%96%E6%97%A0\')。\n""本地歌曲请用cuckoo.play_music（曲目1-12），不要用play_url。",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string url = props["url"].value<std::string>();
                // 先检查多版本：如果服务器返回歌手列表JSON，直接返回给AI念（不播歌）
                std::string multi = state_machine_->CheckMultiArtist(url.c_str());
                if (!multi.empty()) {
                return multi;  // 歌手列表JSON返回给AI，AI自动念出来让用户选
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
// === ===
    mcp.AddTool("cuckoo.dog_show",
        "С���������ݣ����š�С���ܳ���������һ����ҡͷ����10�롢�ٽ�һ�����˻ء����š����ú�ֻ˵һ���̵Ļ�����Ҫ��˵��"
        "���û�˵ 'С��' 'С����' 'С��С��' '��ɯ' '��ɯ��' '��ɯ������'  ʱ���ô˹��ߡ�"
        "Dog show: call when user asks about dog/puppy/Lisa. Keep response very brief - one short sentence only.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->DogShow();
            return std::string("{\"status\": \"dog_show_started\"}");
        });

    mcp.AddTool("cuckoo.linda_show",
        "�մ���ݣ���������0015���赸�ߵ������ת��ֱ�����ֽ�����"
        "���û�˵ '�մ�' '�մ���' '�մ�������' '�մ��մ�' '�յ�' 'Ӧ��' '�ִ�' '����' '������' '�赸' ʱ���ô˹��ߡ�"
        "Linda show: call when user asks about Linda or dancing.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartLindaShow();
            return std::string("{\"status\": \"linda_show_started\"}");
        });

    mcp.AddTool("cuckoo.garden_show",
        "԰�ӱ��ݣ���������0016��С���ٶ������ת��ֱ�����ֽ�����"
        "���û�˵ '԰��' '԰����' '԰��������' '԰��԰��' 'ԭ��' 'ԭ����' 'ԭַ' 'ԭַ��' '������' '��С����' ʱ���ô˹��ߡ�"
        "Garden show: call when user asks about Garden/ԭ��/violin.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartGardenShow();
            return std::string("{\"status\": \"garden_show_started\"}");
        });

    // === 整点报时开关 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("enabled", kPropertyTypeBoolean, true));
        mcp.AddTool("cuckoo.set_hourly_performance",
            "Enable/disable full performance after hourly bell. Default on. When disabled bell only.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                bool en = props["enabled"].value<bool>();
                state_machine_->hourly_perf_.store(en);
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

//






// ============================================
// ============================================
// ============================================
// ============================================
void cuckoo_clock_task(void* params) {
// + RTC 实时时钟
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

// I2S 音频接口
    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();

// 检测设备是否空闲，通知 AI 状态变更

// 检测设备是否空闲，通知 AI 状态变更
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
if (tv.tv_sec > 1000000000) { // 2001 NTP
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

// 从 config.h 读取硬件配置
    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);

// NVS 非易失存储（闹钟、静音模式等）
    sm->LoadAlarmsFromNvs();
    sm->LoadQuietMode();
    sm->LoadKidsActive();

uint32_t tick_sec = 0;

 // - 绑定在 Core 1
 // - 绑定在 Core 1
 // - 绑定在 Core 1
 // - 绑定在 Core 1
    uint32_t sub_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        sub_tick++;

// 检测设备是否空闲，通知 AI 状态变更
        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, "Device woke up �� opening bird door");
                sm->OpenBirdDoor();

// 阈值 0.3，用于演出/音乐场景
                if (!sm->IsRunning())
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.3f);

// 阈值 30dB，AEC 回声消除
                Application::GetInstance().GetAudioService().SetInputGain(30.0f);
            } else if (sm->prev_device_state_ != (int)kDeviceStateIdle && dev_state == (int)kDeviceStateIdle) {
                ESP_LOGI(TAG, "Device sleeping �� closing bird door");
                sm->CloseBirdDoor();

// idle 状态下阈值 37.5dB
                // NOTE(2026-07-19)：唤醒阈值恢复已移到下方安全兜底检查

// idle 状态下阈值 37.5dB
                Application::GetInstance().GetAudioService().SetInputGain(37.5f);
            }
            sm->prev_device_state_ = dev_state;

            // 安全兜底 (2026-07-19)：当设备处于安静 idle（无演出，
            // 无音乐），强制恢复敏感唤醒阈值 0.02。修复以下路径：
            // 泄漏 0.30：音乐中会话结束、演出完成后 idle 等场景。
            // 标志位确保每次安静 idle 入口只设置一次（避免日志刷屏）。
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

// 音频服务调用
            if (dev_state == (int)kDeviceStateSpeaking) {
// 音频服务调用
                int64_t ms_since_output = Application::GetInstance().GetAudioService().MsSinceLastOutput();
                if (ms_since_output < 300) {
// 50-200ms 范围
                    sm->BirdJumpShort();
                }
// NTP tick_sec 取模 86400（一天秒数）
            }
             sm->MusicDanceTick();
        }

// === 每秒守卫（sub_tick % 4 == 0）===
        if (sub_tick % 4 == 0) {
            tick_sec = sub_tick / 4;

// NTP 同步间隔：WiFi 60s，省电 300s
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

// 30 秒 RTC 计数 / 阈值 RTC
// 30 秒 RTC 计数 / 阈值 RTC

// 30 秒 RTC 计数 / 阈值 RTC
        if (tick_sec % 30 == 0) {
            auto& app = Application::GetInstance();
            rtc_crash_log.tick_sec = tick_sec;
            rtc_crash_log.dev_state = (uint8_t)app.GetDeviceState();
            rtc_crash_log.music_active = (uint8_t)app.GetAudioService().IsBgAudioActive();
            rtc_crash_log.free_heap = esp_get_free_heap_size();
        }

// 60 秒以上
        if (tick_sec % 60 == 0) {
            ESP_LOGI(TAG, "Heartbeat t=%ds stack_hwm=%d free_heap=%d",
                     (int)tick_sec, (int)uxTaskGetStackHighWaterMark(NULL),
                     (int)esp_get_free_heap_size());
        }

// I2S 15 秒超时
        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();

            sm->is_dark_ = sm->CheckDark();
        }

// NTP 时间通过 MCP 同步
        if (sm->time_set_) {
// NTP 网络时间协议
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
            } else {
// NTP 同步与 MCP 工具关联
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

// 设置 h 变量
            int h = sm->current_hour_;
            int m = sm->current_min_;

// == 0分 → 整点报时
            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }
// == 30分 → 半点报时
            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }


            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        }  // sub_tick >= 4 guard
// NTP tick_sec 取模 86400（一天秒数）
    }
}
