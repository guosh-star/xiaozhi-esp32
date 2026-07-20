#include "cuckoo_controller.h"
#include "cuckoo_bell_sound.h"
#include "../../mcp_server.h"
#include "../../application.h"
#include "../../boards/common/board.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_http_client.h>
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


// Helper: convert server /stream or /opus URL to /pcm URL for ESP32 background audio
static void ConvertToPcmUrl(char* url, size_t url_sz) {
    // /stream?q=... → /pcm?q=...
    char* pos = strstr(url, "/stream?");
    if (pos) {
        // "/stream" = 7 chars, "/pcm" = 4 chars, keep "?..." at pos+7
        size_t tail = strlen(pos + 7) + 1;
        memmove(pos + 4, pos + 7, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // /opus?q=... → /pcm?q=...
    pos = strstr(url, "/opus?");
    if (pos) {
        // "/opus" = 6 chars, "/pcm" = 4 chars, keep "?..." at pos+5
        size_t tail = strlen(pos + 5) + 1;
        memmove(pos + 4, pos + 5, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // Also handle without leading slash (AI sometimes sends "stream?q=..." or "opus?q=...")
    pos = strstr(url, "stream?");
    if (pos && (pos == url || *(pos-1) != '/')) {
        // "stream" = 6 chars, "pcm" = 3 chars, keep "?..." at pos+6
        size_t tail = strlen(pos + 6) + 1;
        memmove(pos + 3, pos + 6, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
    pos = strstr(url, "opus?");
    if (pos && (pos == url || *(pos-1) != '/')) {
        // "opus" = 4 chars, "pcm" = 3 chars, keep "?..." at pos+4
        size_t tail = strlen(pos + 4) + 1;
        memmove(pos + 3, pos + 4, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
}
#define TAG "CuckooCtrl"

// ============================================
// LEDC PWM 通道分配
// ============================================
// TB6612 需�?4 �?PWM 通道
// 舵机需�?2 �?PWM 通道
// 鸟门电机 1 �?PWM 通道
// 总共 7 �?// TB6612 PWM 要求 ~10-100KHz
// 舵机要求 50Hz
// 鸟门电机 L9110S 要求 ~1-10KHz

// ============================================
// Motor (TB6612 / DRV8833)
// ============================================

// TB6612 构造: pwm_pin + 2x方向脚 + 1个LEDC通道
Motor::Motor(gpio_num_t pwm_pin, gpio_num_t in1_pin, gpio_num_t in2_pin, ledc_channel_t channel)
    : pwm_pin_(pwm_pin), in1_pin_(in1_pin), in2_pin_(in2_pin), speed_(0), reverse_(false),
      ledc_channel_(channel), ledc_channel2_(LEDC_CHANNEL_MAX), driver_type_(kTB6612) {

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << in1_pin_) | (1ULL << in2_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // LEDC_TIMER_0 只配置一次
    static bool motor_timer_inited = false;
    if (!motor_timer_inited) {
        ledc_timer_config_t timer_conf = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 10000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer_conf);
        motor_timer_inited = true;
    }

    ledc_channel_config_t ch_conf = {
        .gpio_num = pwm_pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    ledc_channel_config(&ch_conf);

    Stop();
}

// DRV8833 构造: 2xPWM脚(in1/in2) + 2个LEDC通道
Motor::Motor(gpio_num_t in1_pin, gpio_num_t in2_pin, ledc_channel_t ch1, ledc_channel_t ch2)
    : pwm_pin_(GPIO_NUM_NC), in1_pin_(in1_pin), in2_pin_(in2_pin), speed_(0), reverse_(false),
      ledc_channel_(ch1), ledc_channel2_(ch2), driver_type_(kDRV8833) {

    // LEDC_TIMER_0 只配置一次
    static bool motor_timer_inited = false;
    if (!motor_timer_inited) {
        ledc_timer_config_t timer_conf = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 10000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer_conf);
        motor_timer_inited = true;
    }

    // 两个 IN 脚都配置为 LEDC PWM 输出
    ledc_channel_config_t ch1_conf = {
        .gpio_num = in1_pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    ledc_channel_config(&ch1_conf);

    ledc_channel_config_t ch2_conf = {
        .gpio_num = in2_pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel2_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    ledc_channel_config(&ch2_conf);

    Stop();
}

void Motor::SetSpeed(int speed) {
    if (speed == 0) { Stop(); return; }
    bool rev = speed < 0;
    int spd = abs(speed);
    if (spd > 100) spd = 100;
    uint32_t duty = (spd * 255) / 100;

    if (driver_type_ == kDRV8833) {
        // DRV8833: PWM 直接打在 IN 脚上
        // AIN1=正转, AIN2=反转 (coast: both LOW; brake: both HIGH)
        if (rev) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel2_, duty);
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, duty);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel2_, 0);
        }
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel2_);
    } else {
        // TB6612: PWMA 输出 PWM, IN1/IN2 控制方向
        gpio_set_level(in1_pin_, rev ? 0 : 1);
        gpio_set_level(in2_pin_, rev ? 1 : 0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
    }
    speed_ = spd;
    reverse_ = rev;
}

void Motor::Stop() {
    if (driver_type_ == kDRV8833) {
        // DRV8833 coast stop (both LOW)
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, 0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel2_, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel2_);
    } else {
        gpio_set_level(in1_pin_, 0);
        gpio_set_level(in2_pin_, 0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
    }
    speed_ = 0;
}

void Motor::Forward(int speed) { SetSpeed(speed); }
void Motor::Reverse(int speed) { SetSpeed(-speed); }

// ============================================
// Servo (SG90)
// ============================================
Servo::Servo(gpio_num_t pin, ledc_channel_t channel)
    : pin_(pin), angle_(90), ledc_channel_(channel) {

    // LEDC_TIMER_1 只配置一次，避免重复初始化
    static bool servo_timer_inited = false;
    if (!servo_timer_inited) {
        ledc_timer_config_t timer_conf = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_14_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 50,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer_conf);
        servo_timer_inited = true;
    }

    ledc_channel_config_t ch_conf = {
        .gpio_num = pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    ledc_channel_config(&ch_conf);

    SetAngle(90);
}

void Servo::SetAngle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    angle_ = angle;
    uint32_t duty = (uint32_t)(409 + (float)(angle) / 180.0f * (2048 - 409));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_);
}

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
// BirdJump (电磁�?
// ============================================
BirdJump::BirdJump(gpio_num_t gpio) : gpio_(gpio), active_(false) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(gpio_, 0);
}

void BirdJump::Jump(int duration_ms) {
    Set(true);
    if (duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        Set(false);
    }
}

void BirdJump::Set(bool on) {
    gpio_set_level(gpio_, on ? 1 : 0);
    active_ = on;
}

// ============================================
// MP3 Player (YX5200/YX5300 UART)
// ============================================
// ============================================
// Mp3Player - 软件解码播放器（异步任务版）
// ============================================

Mp3Player::~Mp3Player() {
    stop_requested_ = true;
    // Give task a chance to exit cleanly (unblock recv, close sockets)
    for (int i = 0; i < 100 && is_playing_; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  // 5s total grace period
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

void Mp3Player::Init(Assets* assets) {
    assets_ = assets;
    ESP_LOGI(TAG, "Mp3Player initialized (async task-based software decode)");
}

// 在播放任务中运行，解码单个 MP3 文件
int Mp3Player::DecodeSingleFile(int index) {
    if (!assets_) {
        ESP_LOGE(TAG, "Assets not initialized");
        return 0;
    }

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

    // 从 assets 获取 MP3 文件数据
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

    // 打开 MP3 解码器
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return 0;
    }

    // 解码信息
    esp_audio_dec_info_t dec_info = {};
    bool info_ready = false;
    int sample_rate = 22050;
    int channels = 1;

    // 解码循环
    uint8_t* input_ptr = (uint8_t*)mp3_data;
    size_t remaining = mp3_size;

    while (remaining > 0 && !stop_requested_) {
        // 准备输入帧
        size_t in_len = (remaining < kInputBufSize) ? remaining : kInputBufSize;
        memcpy(input_buf_, input_ptr, in_len);

        esp_audio_dec_in_raw_t raw;
        raw.buffer = input_buf_;
        raw.len = in_len;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        // 准备输出帧
        esp_audio_dec_out_frame_t frame;
        frame.buffer = output_buf_;
        frame.len = kOutputBufSize;
        frame.needed_size = 0;
        frame.decoded_size = 0;

        // 解码
        ret = esp_mp3_dec_decode(mp3_dec_handle_, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK) {
            // 首次获取解码信息
            if (!info_ready) {
                esp_audio_err_t info_ret = esp_audio_dec_get_info(mp3_dec_handle_, &dec_info);
                if (info_ret == ESP_AUDIO_ERR_OK) {
                    sample_rate = dec_info.sample_rate;
                    channels = dec_info.channel;
                    ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %.1f kbps",
                             sample_rate, channels, dec_info.bitrate / 1000.0f);
                }
                info_ready = true;
            }

            // 有解码出的 PCM 数据，推给扬声器
            if (frame.decoded_size > 0 && !stop_requested_) {
                int16_t* pcm_data = (int16_t*)frame.buffer;
                size_t num_samples = frame.decoded_size / sizeof(int16_t);

                // 计算这段 PCM 的播放时长
                int play_ms = (int)(num_samples * 1000 / sample_rate / (channels > 0 ? channels : 1));
                if (play_ms < 10) play_ms = 10;  // 最小延时

                auto& app = Application::GetInstance();

                // ---- Smooth ducking: fade music out when AI starts speaking ----
                if (app.GetDeviceState() == kDeviceStateSpeaking) {
                    if (ducking_gain_ >= 1.0f) {
                        ducking_gain_ = 1.0f;
                        ducking_start_us_ = esp_timer_get_time();
                        ESP_LOGI(TAG, "DecodeSingleFile: AI speaking, fading out");
                    }
                }

                if (ducking_gain_ < 1.0f) {
                    // AI was speaking but stopped (false trigger) → restore
                    if (app.GetDeviceState() != kDeviceStateSpeaking) {
                        ducking_gain_ = 1.0f;
                        ducking_start_us_ = 0;
                        ESP_LOGI(TAG, "DecodeSingleFile: false trigger, restored");
                    } else {
                        // Calculate fade progress
                        int64_t elapsed = esp_timer_get_time() - ducking_start_us_;
                        ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
                        if (ducking_gain_ < 0.2f) {
                            ducking_gain_ = 0.2f;  // Hold at 20% as background
                        }
                    }
                }

                // Apply gain + clip
                float gain = ducking_gain_;
                for (size_t i = 0; i < num_samples; i++) {
                    int32_t s = (int32_t)(pcm_data[i] * gain);
                    if (s > 30000) s = 30000;
                    if (s < -30000) s = -30000;
                    pcm_data[i] = (int16_t)s;
                }

                app.GetAudioService().OutputRawPcm(pcm_data, num_samples, sample_rate);

                // 等待播放完成（OutputRawPcm 非阻塞）
                vTaskDelay(pdMS_TO_TICKS(play_ms));
            }

            // 移动输入指针
            size_t consumed = raw.consumed;
            if (consumed == 0) {
                consumed = 1;  //  Tiny files advance byte-by-byte
                if (consumed > remaining) consumed = remaining;
            }
            input_ptr += consumed;
            remaining -= consumed;

        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer too small, needed %lu", (unsigned long)frame.needed_size);
            break;
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            ESP_LOGE(TAG, "Unsupported MP3 format (stopping)");
            break;
        } else {
            // 解码错误：跳过当前帧
            ESP_LOGW(TAG, "Decode error %d, skipping frame", ret);
            size_t skip = raw.consumed ? raw.consumed : 1;
            if (skip > remaining) skip = remaining;
            input_ptr += skip;
            remaining -= skip;
        }
    }

    // 关闭解码器
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    // 恢复小智的 Opus 解码输出（无论正常结束还是被打断都必须恢复）
    auto& app = Application::GetInstance();
    if (stop_requested_) {
        app.GetAudioService().FlushOutputDma();  // clear stale I2S data before AI speaks
    }
    app.GetAudioService().SetOutputMuted(false);

    ESP_LOGI(TAG, "Finished playing %s (stopped=%d)", filename, stop_requested_.load() ? 1 : 0);
    return !stop_requested_;
}

// ============================================
// PlayUrl - HTTP下载音频并播放
// 支持:
//   1. 直接 MP3 URL (Content-Type: audio/*)
//   2. 代理服务 /stream 端点 (直接返回音频)
//   3. 代理服务 JSON 响应 (提取 url 字段后重试)
// ============================================
int Mp3Player::PlayUrl(const char* url) {
    if (!url || url[0] == '\0') return -1;

    ESP_LOGI(TAG, "PlayUrl: launching background task for %s", url);

    // 后台任务上下文
    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PlayUrlCtx();
    ctx->self = this;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->url[sizeof(ctx->url) - 1] = '\0';

    xTaskCreate(PlayUrlTask, "mp3_url", 8192, ctx, 5, NULL);
    return 0;  // 非阻塞，立即返回
}

// 严格 MPEG 帧头校验：不仅检查 FF Ex，还校验 bitrate/samplerate/layer
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

    // HTTP 客户端
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

    // 解码器
    void* dec_handle = nullptr;
    esp_audio_err_t dec_ret = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PlayUrl: decoder fail %d", dec_ret);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }

    self->is_playing_ = true;
    app.GetAudioService().SetOutputMuted(true);

    // 批量缓冲：如果Content-Length已知，用少量大批次（降低帧边界错位机会）
    size_t batch_size = 256 * 1024;
    if (content_length > 0 && content_length < 8 * 1024 * 1024) {
        batch_size = content_length; // 全量下载
        if (batch_size > 4 * 1024 * 1024) batch_size = 4 * 1024 * 1024; // 单批上限4MB
        ESP_LOGI(TAG, "PlayUrl: batch=%dKB (content=%dKB)", (int)(batch_size/1024), content_length/1024);
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        // PSRAM分配失败→回退到256KB再试
        batch_size = 256 * 1024;
        buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    }
    if (!buf) {
        batch_size = 64 * 1024;
        buf = (uint8_t*)malloc(batch_size);
    }
    if (!buf) {
        batch_size = 64 * 1024;
        buf = (uint8_t*)malloc(batch_size);
    }
    if (!buf) {
        ESP_LOGE(TAG, "PlayUrl: alloc failed");
        esp_mp3_dec_close(dec_handle); esp_http_client_close(client); esp_http_client_cleanup(client);
        self->is_playing_ = false; app.GetAudioService().SetOutputMuted(false);
        vTaskDelete(NULL); return;
    }

    // 重采样缓冲区（立体声→单声道 + 采样率转换用）
    const int kOutRate = 24000;
    // Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000→24000
    //   mono_ns = 1152, ratio_max = 24000/8000 = 3.0, out_ns_max = 3456
    // Use 4096 to be safe with nearest-neighbor upsample
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
    // 第一批：下载 + 预处理（跳过ID3标签 + 解析采样率）
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

    // ---- 跳过 ID3v2 标签 ----
    mp3_start = 0;
    if (batch_len > 10 && memcmp(buf, "ID3", 3) == 0) {
        uint32_t id3_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14)
                          | ((buf[8] & 0x7F) << 7)  |  (buf[9] & 0x7F);
        mp3_start = 10 + id3_size;
        if (mp3_start > batch_len - 1024) mp3_start = 0; // 标签太大/跨批，不做跳过
        ESP_LOGI(TAG, "PlayUrl: ID3v2 %luB, MP3 @ %u", id3_size, (unsigned)mp3_start);
    }

    // ---- 从原始数据扫描 MPEG 帧头获取采样率 ----
    for (size_t k = mp3_start; k + 3 < batch_len; k++) {
        if (buf[k] == 0xFF && (buf[k+1] & 0xE0) == 0xE0) {
            int ver = (buf[k+1] >> 3) & 0x03;
            int lay = (buf[k+1] >> 1) & 0x03;
            if (lay != 1) continue; // 只要 Layer 3
            int sri = (buf[k+2] >> 2) & 0x03;
            if      (ver == 3) { static const int r[]={44100,48000,32000,0}; sample_rate = r[sri]; }
            else if (ver == 2) { static const int r[]={22050,24000,16000,0}; sample_rate = r[sri]; }
            else if (ver == 0) { static const int r[]={11025,12000,8000,0};  sample_rate = r[sri]; }
            if (sample_rate > 0) { ESP_LOGI(TAG, "PlayUrl: MP3 %dHz @ %u", sample_rate, (unsigned)k); break; }
        }
    }
    if (sample_rate == 0) { sample_rate = 44100; ESP_LOGW(TAG, "PlayUrl: no sync header, default %dHz", sample_rate); }

    // ---- 解码循环（批量流式）----
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
                    // ---- Smooth ducking: fade when AI starts speaking ----
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

                    // Mix stereo→mono + resample to 24000Hz if needed
                    // OutputRawPcm's internal resampler treats stereo as mono (cross-channel garbling)
                    if (sample_rate != kOutRate && mono_ns > 0) {
                        // Step 1: stereo→mono (average L/R)
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }
                        // Step 2: linear resample→kOutRate
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
                        // Apply ducking gain to output buffer
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
                        // Apply ducking gain
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
                // 用解码器返回的 frame_size 代替 raw.consumed 确保帧边界精确
                size_t c = dec_info.frame_size;
                if (c == 0) c = raw.consumed;  // 回退
                if (c == 0) c = 1;
                if (c >= rem) { rem = 0; } else { ptr += c; rem -= c; }
            } else if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (raw.consumed > 0 && raw.consumed < rem) { ptr += raw.consumed; rem -= raw.consumed; }
                else if (raw.consumed == 0) {
                    // 数据起始不在帧头，跳过至下一个 MPEG 同步帧（防止死循环）
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 < rem) { ptr += scan; rem -= scan; }
                    else { rem = 0; break; }  // 未找到同步帧，放弃并 carry 到下一批
                }
                if (rem < 1024) break;
            } else {
                if (raw.consumed > 0) {
                    if (raw.consumed >= rem) { rem = 0; }
                    else { ptr += raw.consumed; rem -= raw.consumed; }
                } else {
                    // 解码失败且未消费→严格跳至下一个 MPEG 帧头
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 >= rem) { rem = 0; break; }
                    ptr += scan; rem -= scan;
                }
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d rem=%u stop=%d", batch_seq, (unsigned)rem, self->stop_requested_.load() ? 1 : 0);
        if (self->stop_requested_) { printf("DD stop_break\n"); break; }

        // ----- 保留上批末尾未消费字节，拼接到下批头部 -----
        size_t carry = rem;
        if (carry > 0 && carry < batch_size) {
            memmove(buf, ptr, carry);
        } else {
            carry = 0;
        }

        // ----- 下载下一批 -----
        batch_len = 0; err_cnt = 0; batch_seq++;
        while (batch_len + carry < batch_size && !self->stop_requested_) {
            int r = esp_http_client_read(client, (char*)(buf + carry + batch_len), batch_size - carry - batch_len);
            if (r > 0) { batch_len += r; err_cnt = 0; }
            else if (r == 0) break;
            else { if (++err_cnt > 3) break; vTaskDelay(pdMS_TO_TICKS(50)); }
        }
        total_dl += batch_len;
        if (batch_len == 0) { ESP_LOGI(TAG, "PlayUrl: batch_len=0 break after %dKB", (int)(total_dl/1024)); break; }
        ESP_LOGI(TAG, "PlayUrl: batch#%d dl=%dKB+%uB carry", batch_seq, (int)(batch_len/1024), (unsigned)carry);
        ptr = buf;
        rem = carry + batch_len;
        // carry=0且非帧头→扫描跳过metadata/非帧数据到下一个严格帧头
        if (carry == 0 && rem > 3 && !IsValidMpegHeader(ptr)) {
            size_t s = 1;
            while (s + 3 < rem && !IsValidMpegHeader(ptr + s)) s++;
            if (s + 3 < rem) {
                ESP_LOGD(TAG, "PlayUrl: skip %d bytes to next frame", (int)s);
                ptr += s; rem -= s;
            } else {
                rem = 0; // 整批无有效帧头，放弃
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

// ── PlayPcm: HTTP下载raw PCM → PushRawPcmToPlayback（走原生播放管线，无帧对齐问题）──
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

    // Download PCM in chunks, push to playback queue
    const size_t CHUNK = sizeof(self->output_buf_);  // never exceed buffer size
    const int SAMPLE_RATE = 24000;
    int64_t t0 = esp_timer_get_time();

    while (self->is_playing_ && !self->stop_requested_) {
        int read = esp_http_client_read(client, (char*)self->output_buf_, CHUNK);
        if (read <= 0) break;

        size_t samples = read / 2;  // 16-bit mono
        int16_t* pcm = (int16_t*)self->output_buf_;

        // ---- Smooth ducking: fade when AI starts speaking ----
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
                self->ducking_gain_ = 1.0f - (float)elapsed / 400000.0f;
                if (self->ducking_gain_ < 0.01f) {
                    self->ducking_gain_ = 0.0f;  // hold at silent, don't kill music
                    ESP_LOGI(TAG, "PlayPcm: fade complete, holding at 0%%");
                }
            }
        }
        // Apply ducking gain
        float g = self->ducking_gain_;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = (int32_t)(pcm[i] * g);
            if (s > 30000) s = 30000;
            if (s < -30000) s = -30000;
            pcm[i] = (int16_t)s;
        }

        app.GetAudioService().PushRawPcmToPlayback(pcm, samples, SAMPLE_RATE);

        // Throttle: yield CPU every ~4s of audio to avoid starving WiFi
        static int bytes_yielded = 0;  // OK: PlayPcmTask is only ever instantiated once
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

// 播放闹钟铃声（PCM 合成，模拟电子闹钟滴滴声，2秒）
// volume: 0.0~1.0，控制音量（渐强用）
// HTTP stream OGG/Opus, feed to OggDemuxer -> PushPacketToDecodeQueue (native pipeline)
int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        Stop();
        // Wait for old task to fully exit (queues are now cleared, so much faster)
        for (int i = 0; i < 300 && is_playing_; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (is_playing_) {
            ESP_LOGW(TAG, "PlayOpus: old task still running, refusing to start");
            return -1;
        }
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
    stop_requested_ = false;  // 清除上次 Stop() 残留的标记
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;

    if (strlen(url) >= 512) {
        ESP_LOGE(TAG, "PlayOpus: URL too long (%zu bytes)", strlen(url));
        return -1;
    }
    struct OpusCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new OpusCtx();
    ctx->self = this;
    strncpy(ctx->url, url, 511);
    ctx->url[511] = '\0';

    xTaskCreatePinnedToCore(PlayOpusTask, "opus_http", 1024 * 24, ctx, 3, NULL, 1);
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

    // Music uses background audio layer — let ducking handle AI interaction.

    // Parse URL
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
    
    // Raw BSD socket
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
    // Try dotted-decimal first, then DNS resolution
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
    
    // Non-blocking connect with 5s timeout (SO_SNDTIMEO doesn't work on lwip connect)
    // Wrapped in a scope block so goto serial_fallback doesn't cross these variable initializations
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
        // fallback path reached via goto, sock already closed
        
        // === Serial fallback ===
        printf("\x01MUSIC_REQ\x02%s\x03\n", url);
        fflush(stdout);
        
        // Clear stop flag (set by Stop() in PlayOpus) so fread loop runs
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
        
                // Read Opus data from UART0 RX with initial delay for relay to fetch
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
                // Prevent AFE power management from disabling MIC
                uint64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_refresh_ms > 8000) {  // every 8s
                // HACK: Prevent audio watchdog timeout during long downloads
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
            if (app.GetDeviceState() == kDeviceStateConnecting) break;  // wake word → stop
            // Double-trigger with ≥500ms gap to avoid false stop from music false-wake
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
    
    // Send HTTP GET
    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    send(sock, req, strlen(req), 0);
    
    // Read response headers
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

    // === Detect format: /opus uses OGG demuxer + main audio pipeline ===
    // === /pcm uses raw bytes pushed to background ring buffer ===
    bool use_opus = (strstr(path, "/opus") != nullptr);
    
    if (use_opus) {
        // ============ Opus path: OGG demux + PushPacketToDecodeQueue ============
        // Same pipeline as AI voice — Opus decoder → playback queue → output
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
        
        // Lower volume during music to reduce AEC bleed → VAD works better
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

            // Ducking: during AI speech, drain data but don't push to decoder.
            // ResetDecoder already cleared the decode queue entering speaking state.
            // Downloaded data is discarded → music skips forward (barely noticeable
            // for a 3–5 min song during a 5–15 s AI response).
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
                continue;  // skip demuxer->Process (data lost, music skips forward)
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
        
        // Restore volume
        if (codec) codec->SetOutputVolume(old_vol);
        ESP_LOGI(TAG, "PlayOpus: volume restored to %d", old_vol);
        
        // Wait for decode queue to drain
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
    
    // ============ PCM path (legacy): raw s16le → PushBackgroundAudio ============
    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    int64_t t0 = esp_timer_get_time(), last_refresh = t0;
    size_t total_dl = 0;
    bool ai_speaking = false;

    app.GetAudioService().SetBackgroundAudioGain(1.0f);
    ESP_LOGI(TAG, "PlayOpus: downloading raw PCM...");

    // === Pre-buffer phase: fill ring buffer before enabling drain ===
    int64_t prebuf_start = esp_timer_get_time();
    while (self->is_playing_ && !self->stop_requested_) {
        int read = recv(sock, buf, CHUNK, 0);
        if (read <= 0) break;
        total_dl += read;
        // Raw s16le PCM: every 2 bytes = 1 sample
        app.GetAudioService().PushBackgroundAudio(
            reinterpret_cast<int16_t*>(buf), read / sizeof(int16_t), 16000);

        size_t fill = app.GetAudioService().GetBgAudioFillLevel();
        if (fill >= 64000) {
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGI(TAG, "PlayOpus: drain enabled (buffered %d samples in %d ms)",
                     (int)fill, (int)((esp_timer_get_time() - prebuf_start) / 1000));
            break;
        }
        if (esp_timer_get_time() - prebuf_start > 10000000) {
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGW(TAG, "PlayOpus: pre-buffer timeout after 10s (%d samples buffered)",
                     (int)fill);
            break;
        }
    }

    // === Main download → push loop ===
    int recv_errors = 0;
    while (self->is_playing_ && !self->stop_requested_) {
        auto state = app.GetDeviceState();
        bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
        if (ai_now != ai_speaking) {
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.3f : 1.0f);
            ESP_LOGI(TAG, "PlayOpus: AI %s, music -> %d%%",
                     ai_now ? "speaking" : "idle", ai_now ? 30 : 100);
        }

        // When AI is speaking, pause TCP download to free WiFi airtime for
        // UDP audio packets (prevents WiFi buffer starvation → TTS stutter).
        // Only pause if buffer sufficient to ride through typical AI reply.
        if (ai_speaking && app.GetAudioService().GetBgAudioFillLevel() > 16000) {
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
            // Server closed connection gracefully
            ESP_LOGI(TAG, "PlayOpus: server closed connection");
            break;
        } else {
            // read < 0: timeout or transient error
            recv_errors++;
            if (recv_errors > 5) {
                ESP_LOGE(TAG, "PlayOpus: recv failed %d times, giving up (errno=%d)", recv_errors, errno);
                break;
            }
            // Wait 1s before retry — WiFi may be reconnecting after AI conversation
            ESP_LOGW(TAG, "PlayOpus: recv error %d/%d (errno=%d), retrying...", recv_errors, 5, errno);
            for (int w = 0; w < 10 && self->is_playing_ && !self->stop_requested_; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            // Refresh power save in case it was changed by channel close
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            continue;
        }

        while (app.GetAudioService().GetBgAudioFillLevel() > 80000 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_refresh > 8000000) {
            // Diagnostic: log buffer fill + download rate every 8s
            size_t fill = app.GetAudioService().GetBgAudioFillLevel();
            int64_t elapsed = now - t0;
            float dl_rate = elapsed > 0 ? (float)total_dl * 1000000.0f / (float)elapsed : 0;
            ESP_LOGI(TAG, "PlayOpus: buf=%d samples dl=%dKB rate=%.1fKB/s",
                     (int)fill, (int)(total_dl / 1024), dl_rate / 1024.0f);
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
    vTaskDelete(NULL);
}

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

    // Generate and play in 100ms chunks — stop_requested_ checked between chunks
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

// 播放任务入口
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
            // play single track - manage mute here
            self->is_playing_ = true;
            int track = self->pending_track_;
            self->pending_track_ = 0;
            Application::GetInstance().GetAudioService().SetOutputMuted(true);
            self->DecodeSingleFile(track);
            self->is_playing_ = false;
            Application::GetInstance().GetAudioService().SetOutputMuted(false);
        }

        if (self->pending_bell_hour_ > 0) {
            // play bell chime using short PCM bell sound (~0.5s each)
            self->is_playing_ = true;
            int hour = self->pending_bell_hour_;
            self->pending_bell_hour_ = 0;
            Application::GetInstance().GetAudioService().SetOutputMuted(true);
            for (int i = 0; i < hour; i++) {
                if (self->stop_requested_) break;
                self->bell_player_.PlayBellSoundSync();
                // pause ~1s between strikes for natural clock sound
                if (i < hour - 1 && !self->stop_requested_) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            self->is_playing_ = false;
            Application::GetInstance().GetAudioService().SetOutputMuted(false);
        }
    }
}

void Mp3Player::PlayTrack(uint8_t folder, uint8_t track) {
    if (folder == 1) {
        // 钟声
        PlayBell(track);
    } else if (folder == 2) {
        // 音乐
        if (track < 1) track = 1;
        if (track > 12) track = 12;
        PlayIndex(track);
    }
}

void Mp3Player::PlayIndex(uint16_t index) {
    if (index < 1) index = 1;
    if (index > 12) index = 12;

    if (!assets_) {
        ESP_LOGE(TAG, "Mp3Player not initialized (call Init first)");
        return;
    }

    // 停止当前播放
    Stop();

    // 清空 stop_requested_ 并设置新曲目
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;

    // 确保任务已创建
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

void Mp3Player::Stop() {
    stop_requested_ = true;
    // Don't set is_playing_=false here - let PlayOpusTask cleanup do it
    // so PlayOpus() can reliably wait for the old task to exit
    pending_track_ = 0;
    pending_bell_hour_ = 0;

    if (mp3_dec_handle_) {
        esp_mp3_dec_reset(mp3_dec_handle_);
    }
    
    // Clear audio queues so WaitForPlaybackQueueEmpty() in old task returns fast
    Application::GetInstance().GetAudioService().ResetDecoder();
}

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

void Mp3Player::PlayBell(int hour) {
    if (hour < 1) hour = 1;
    if (hour > 12) hour = 12;

    if (!assets_) return;

    // 停止当前播放
    Stop();

    // 设置钟声重复次数
    pending_bell_hour_ = hour;

    // 确保任务已创建
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

void Mp3Player::PlayBgMusic(int index) {
    if (index < 1) index = 1;
    if (index > 12) index = 12;
    PlayIndex(index);
}

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

    // Overallocate: MP3 worst-case PCM ratio ~2.75x, 4x is very safe
    size_t max_samples = mp3_size * 4;
    if (max_samples < 8192) max_samples = 8192;
    int16_t* buf = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu samples in PSRAM", (unsigned long)max_samples);
        return -1;
    }

    // Open decoder
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

    esp_audio_dec_info_t dec_info = {};
    int sample_rate = 22050;
    uint8_t* input_ptr = (uint8_t*)mp3_data;
    size_t remaining = mp3_size;
    size_t written = 0;

    // Single-pass decode
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
                // Decoder buffered all input; no progress possible, stop
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

    // Close decoder
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
    ESP_LOGI(TAG, "Decoded %s to PCM: %zu samples, %d Hz", filename, written, sample_rate);
    return 0;
}

// ============================================
// LDR 光敏传感器
// ============================================
LdrSensor::LdrSensor(gpio_num_t adc_pin, int threshold)
    : adc_pin_(adc_pin), threshold_(threshold) {
    ESP_LOGI(TAG, "LDR sensor on GPIO %d, threshold %d", adc_pin_, threshold_);
}

int LdrSensor::ReadRaw() {
    int level = gpio_get_level(adc_pin_);
    return level == 1 ? 4095 : 0;
}

bool LdrSensor::IsDark() { return ReadRaw() > threshold_; }
void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }

// ============================================
// BellSoundPlayer - 通过 AI 音频系统播放布谷鸟叫�?// ============================================
void BellSoundPlayer::PlayCuckooSoundSync() {
    // 直接写 I2S（阻塞直到播完），使用 data_if_mutex_ 与 AudioOutputTask 互斥
    // 不需要 SetOutputMuted，也不需要 vTaskDelay
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_wake_sound,
        CUCKOO_WAKE_SOUND_NUM_SAMPLES,
        CUCKOO_WAKE_SOUND_SAMPLE_RATE
    );
}

void BellSoundPlayer::PlayBellSoundSync() {
    // 直接写 I2S（阻塞直到播完），使用 data_if_mutex_ 与 AudioOutputTask 互斥
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_bell_sound,
        CUCKOO_BELL_SOUND_NUM_SAMPLES,
        CUCKOO_BELL_SOUND_SAMPLE_RATE
    );
}

// ============================================
// CuckooStateMachine
// ============================================
CuckooStateMachine::CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                                        Motor* bird_door, Servo* violin, Servo* dog,
                                        BirdJump* bird_jump, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        RtcPcf8563* rtc, LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
      bird_door_(bird_door), violin_servo_(violin), dog_servo_(dog),
      bird_jump_(bird_jump), mp3_(mp3), bell_player_(bell_player), rtc_(rtc), ldr_(ldr),
      motor_power_pin_(GPIO_NUM_NC),
      current_performance_(kPerformanceNone), current_phase_(kPhaseIdle),
      call_count_(0), total_calls_(0), is_running_(false),
      last_hour_(-1), last_half_hour_(-1), show_music_index_(0),
      current_hour_(12), current_min_(0), current_sec_(0), time_set_(false),
      is_dark_(false) {
    last_idle_exit_us_ = 0;
    prev_device_state_ = -1;
    // 电机电源P-MOSFET控制 (GPIO LOW=ON, HIGH=OFF)
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);
    MotorPowerOff();  // 默认断电, 100K上拉到5V
  }

CuckooStateMachine::~CuckooStateMachine() {
    StopAll();
    MotorPowerOff();
}

void CuckooStateMachine::MotorPowerOn() {
    gpio_set_level(motor_power_pin_, 0);
}

void CuckooStateMachine::MotorPowerOff() {
    gpio_set_level(motor_power_pin_, 1);
}

void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();

    // 防AI误触发：设备刚从idle唤醒5秒内，跳过 performance（AI可能在唤醒时自动调cuckoo.performance）
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {  // 5秒窗口
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }

    // 如果 AI 正在说话/监听，先打断它
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
        vTaskDelay(pdMS_TO_TICKS(200));  // 等状态机切换到 idle
    }
    // 表演期间禁掉语音处理，防止钟声被误认为唤醒词唤醒 AI
    app.GetAudioService().EnableVoiceProcessing(false);
    // 显式禁掉唤醒词检测（旧版本 EnableVoiceProcessing 可能不处理此逻辑）
    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
            total_calls_ = hour;       // 记录是几点（敲钟用）
            call_count_ = 3;           // 固定叫三声
            break;
        case kPerformanceHalf:
            total_calls_ = 0;
            call_count_ = 3;           // 半点也固定叫三声
            break;
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
        default: is_running_ = false; return;
    }
    // 启动独立任务执行表演（线性执行，无竞态）
    xTaskCreate(
        PerformanceTask,
        "cuckoo_perf",
        4096,
        this,
        3,
        nullptr
    );
}


void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Performance task started");

    // 静音 AI 输出，清空播放队列，防止钟声和 Opus 混叠
    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    // ====== 整点报时简化版（电机无安装，只走音频流程） ======
    if (sm->current_performance_ == kPerformanceHour) {
        // Phase 1: 布谷鸟叫声 x3 (cuckoo.mp3 或 BellSoundPlayer)
        for (int i = 0; i < sm->call_count_; i++) {
            if (sm->bird_jump_) sm->bird_jump_->Jump(150);
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundSync();
            }
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }

        // Phase 2: bell chime - 解码0013.mp3到PCM缓冲区，循环播避免MP3解码器抖动
        // 一次性解码到PSRAM，用OutputRawPcm重复播放，跳过MP3解码器反复打开的抖动
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            int16_t* bell_pcm = nullptr;
            size_t bell_samples = 0;
            int bell_sr = 0;
            int dec_ret = sm->mp3_->DecodeToBuffer(13, &bell_pcm, &bell_samples, &bell_sr);
            if (dec_ret == 0 && bell_pcm && bell_samples > 0) {
                ESP_LOGI(TAG, "Bell PCM loaded: %zu samples @ %d Hz", bell_samples, bell_sr);
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    app.GetAudioService().OutputRawPcm(bell_pcm, bell_samples, bell_sr);
                    // OutputRawPcm 是阻塞的，播完才返回
                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                heap_caps_free(bell_pcm);
            } else {
                // 回退：用 PlayIndex
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


        // Phase 3: 播放对应整点的音乐，最长 5 分钟
        // 背景音乐音量略低于当前音量（保持区分，不依赖默认值）
        auto* codec = Board::GetInstance().GetAudioCodec();
        int old_volume = codec ? codec->output_volume() : 70;
        int music_vol = old_volume - 5;
        if (music_vol < 10) music_vol = 10;
        if (codec) codec->SetOutputVolume(music_vol);
        
        // 先恢复 AI 输出，支持音乐用 Mp3Player（不走 AudioService I2S）
        app.GetAudioService().SetOutputMuted(false);
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            sm->mp3_->PlayBgMusic(sm->total_calls_);
            // 等音乐开始
            int wait_start = 0;
            while (wait_start < 6 && sm->is_running_ && !sm->mp3_->IsPlaying()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                wait_start++;
            }
            // 等音乐自然播完（IsPlaying 播完自动变 false），加兜底超时防止异常
            int max_wait_sec = 3 * 60;  // 最长等 3 分钟（兜底）
            int elapsed = 0;
            while (sm->is_running_ && sm->mp3_->IsPlaying() && elapsed < max_wait_sec) {
                vTaskDelay(pdMS_TO_TICKS(500));
                elapsed++;
            }
            if (elapsed >= max_wait_sec) {
                ESP_LOGW(TAG, "Hourly chime music timed out (3 min), stopping");
                sm->mp3_->Stop();
            }
        }
        // 恢复音量
        if (codec) codec->SetOutputVolume(old_volume);

    // ====== 半点报时：布谷鸟叫声 x3，无音乐无敲钟 ======
    } else if (sm->current_performance_ == kPerformanceHalf) {
        for (int i = 0; i < sm->call_count_; i++) {
            if (sm->bird_jump_) sm->bird_jump_->Jump(150);
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundSync();
            }
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        // 半点结束后恢复 AI
        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== Done ======
    if (sm->mp3_) sm->mp3_->Stop();
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    sm->current_phase_ = kPhaseIdle;
    sm->MotorPowerOff();
    // 恢复语音处理和唤醒词检测
    app.GetAudioService().EnableVoiceProcessing(true);
    app.GetAudioService().EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    is_dark_ = dark;
    if (is_running_) return;

    // AI 忙时不报时（说话/监听/连接中等）
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Skipping chime: device busy (state=%d)", state);
        return;
    }

    // 夜间模式（20:00-7:00）不报时
    if ((is_dark_ && hour >= 20) || hour < 7) {
        return;
    }

    // 由调用方决定是整点报时还是半点报时
    // 使用 last_hour_ 和 last_half_hour_ 防重复（由 cuckoo_clock_task 管理）

    // 整点报时
    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting performance", hour, min);
        StartPerformance(kPerformanceHour, hour);
        MarkHourlyChime(hour);  // dedup after successful trigger
    }
    // 半点报时
    else if (min == 30) {
        ESP_LOGI(TAG, "Half-hour chime: %d:%02d, starting mini performance", hour, min);
        StartPerformance(kPerformanceHalf, hour);
        MarkHalfHourlyChime(hour);  // dedup after successful trigger
    }
}

void CuckooStateMachine::SetTime(int hour, int min, int sec) {
    current_hour_ = hour % 24;
    current_min_ = min % 60;
    current_sec_ = sec % 60;
    time_set_ = true;
    ESP_LOGI(TAG, "Time set to %02d:%02d:%02d", current_hour_.load(), current_min_.load(), current_sec_.load());
}

void CuckooStateMachine::GetTime(int &hour, int &min) {
    hour = current_hour_;
    min = current_min_;
}

// ============================================
// 闹钟功能
// ============================================
void CuckooStateMachine::SetAlarm(int hour, int minute, bool repeat_daily) {
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (alarm_count_ >= kMaxAlarms) {
        ESP_LOGW(TAG, "Alarm list full (max %d)", kMaxAlarms);
        return;
    }
    // Check for duplicate - update existing
    for (int i = 0; i < alarm_count_; i++) {
        if (alarms_[i].hour == hour && alarms_[i].minute == minute) {
            alarms_[i].enabled = true;
            alarms_[i].repeat_daily = repeat_daily;
            ESP_LOGI(TAG, "Alarm updated: %02d:%02d (repeat=%d)", hour, minute, repeat_daily);
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
}

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

bool CuckooStateMachine::DeleteAlarm(int index) {
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (index < 1 || index > alarm_count_) {
        ESP_LOGW(TAG, "Invalid alarm index: %d (have %d alarms)", index, alarm_count_.load());
        return false;
    }
    // Shift remaining alarms down
    for (int i = index - 1; i < alarm_count_ - 1; i++) {
        alarms_[i] = alarms_[i + 1];
    }
    alarm_count_--;
    ESP_LOGI(TAG, "Alarm %d deleted (remaining: %d)", index, alarm_count_.load());
    return true;
}

void CuckooStateMachine::StopAlarm() {
    alarm_stopped_ = true;
    alarm_ringing_ = false;
    // For one-shot alarms, disable after user stops it
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
    // Stop any playing audio
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

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

void CuckooStateMachine::AlarmTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Alarm task started");

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    while (sm->alarm_ringing_ && !sm->alarm_stopped_) {
        // Play alarm ringtone x50 (2s each = 100s total ringing)
        // First round: ramp volume 15%→100% over first 10 calls (20s)
        for (int i = 0; i < 50; i++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            if (sm->mp3_) {
                float volume;
                if (i < 10) {
                    volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% → 100% over first 10
                } else {
                    volume = 1.0f;
                }
                sm->mp3_->PlayAlarmRing(volume);
            }
        }

        if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;

        // Snooze: wait 2 minutes, checking stopped_ every second
        ESP_LOGI(TAG, "Alarm snoozing for 2 minutes...");
        for (int s = 0; s < 120; s++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Cleanup
    app.GetAudioService().SetOutputMuted(false);
    sm->alarm_ringing_ = false;
    sm->alarm_stopped_ = false;
    ESP_LOGI(TAG, "Alarm task ended");
    vTaskDelete(NULL);
}

void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Dance start (Arduino-style)");

    // M1 舞蹈电机: 正转随机1-3秒 → 停 → 反转随机1-3秒 → 循环
    // M2 小提琴: 旋转
    // 小提琴舵机: 90°→0°→180°→90° 扫描
    unsigned long start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long end = start + 8000; // 总时长8秒

    int m1_stage = 0;
    unsigned long m1_timer = start;
    int m1_rand_time = 1000 + (esp_random() % 2001); // 1-3秒

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS) < end) {
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // M1 舞蹈电机: 正转/反转交替
        switch (m1_stage) {
            case 0:
                if (m1_) m1_->Forward(80);
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
                if (m1_) m1_->Reverse(80);
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

        // M2 小提琴电机: 持续正转
        if (m2_) m2_->Forward(60);

        // 小提琴舵机: 90→0→180→90 循环摆动
        if (violin_servo_) violin_servo_->Sweep(45, 135, 1500);

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (m1_) m1_->Stop();
    if (m2_) m2_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    is_running_ = false;
    MotorPowerOff();
    ESP_LOGI(TAG, "Dance finished");
}


void CuckooStateMachine::StartShow() {
    // 整点/半点报时任务正在运行 → 不允许 Show 打断（AI 误调保护）
    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }
    // 其他情况（如闹钟）可以先停再开始
    if (is_running_) {
        StopAll();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Show starting: dance + waterwheel + music");

    // 用独立任务线性执行，不阻塞MCP调用线程
    xTaskCreate(
        ShowTask,
        "cuckoo_show",
        4096,
        this,
        3,
        nullptr
    );
}

void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Show task started");

    // 开门 + 鸟跳出
    if (sm->bird_door_) {
        sm->bird_door_->Forward(80);
        vTaskDelay(pdMS_TO_TICKS(500));
        sm->bird_door_->Stop();
    }
    if (sm->bird_jump_) sm->bird_jump_->Jump(200);

    // 选音乐
    int next = (esp_random() % 12) + 1;
    if (next == sm->show_music_index_ && next < 12) {
        next++;
    } else if (next == sm->show_music_index_) {
        next = 1;
    }
    sm->show_music_index_ = next;
    if (sm->mp3_) sm->mp3_->PlayBgMusic(next);

    // 等音乐开始播放（最多等3秒）
    int wait_start = 0;
    while (wait_start < 6 && sm->is_running_ && !sm->mp3_->IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_start++;
    }

    // === M1 舞蹈电机：正转/反转交替（Arduino runMotor1 移植） ===
    unsigned long m1_timer = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001); // 1-3秒

    while (sm->is_running_ && sm->mp3_->IsPlaying()) {
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        switch (m1_stage) {
            case 0:
                if (sm->m1_) sm->m1_->Forward(80);
                if (now - m1_timer > m1_rand_time) { m1_timer = now; m1_stage = 1; }
                break;
            case 1:
                if (sm->m1_) sm->m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 2;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
            case 2:
                if (sm->m1_) sm->m1_->Reverse(80);
                if (now - m1_timer > m1_rand_time) { m1_timer = now; m1_stage = 3; }
                break;
            case 3:
                if (sm->m1_) sm->m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 停止 + 关门
    if (sm->m1_) sm->m1_->Stop();
    if (sm->bird_door_) {
        sm->bird_door_->Reverse(80);
        vTaskDelay(pdMS_TO_TICKS(500));
        sm->bird_door_->Stop();
    }

    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    sm->MotorPowerOff();

    Application::GetInstance().GetAudioService().EnableVoiceProcessing(true);
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);

    ESP_LOGI(TAG, "Show task finished");
    vTaskDelete(NULL);
}

void CuckooStateMachine::PlayMusic(int index) {
    if (mp3_) mp3_->PlayBgMusic(index);
}

void CuckooStateMachine::StopAll() {
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
    if (m1_) m1_->Stop();
    if (m2_) m2_->Stop();
    if (m3_) m3_->Stop();
    if (m4_) m4_->Stop();
    if (bird_door_) bird_door_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    if (dog_servo_) dog_servo_->SetAngle(90);
    if (bird_jump_) bird_jump_->Set(false);
    // 背景音乐正在正常播放时不杀歌 — 防止 AI 误判网络卡后
    // 调用 stop_all+play_url 连环重放打断音乐
    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
            mp3_->Stop();
        }
    }

    // 通过 Schedule 在主循环中安全切 idle + 恢复唤醒检测
    // 不直接 AbortSpeaking — TTS 可能正在正常播放，让它自然结束
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

void CuckooStateMachine::StopMusic() {
    if (mp3_) {
        mp3_->Stop();
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

void CuckooStateMachine::OpenDoor() {
    MotorPowerOn();
    if (bird_door_) {
        bird_door_->Forward(80);
        vTaskDelay(pdMS_TO_TICKS(1500));
        bird_door_->Stop();
    }
    MotorPowerOff();
}

void CuckooStateMachine::CloseDoor() {
    MotorPowerOn();
    if (bird_door_) {
        bird_door_->Reverse(80);
        vTaskDelay(pdMS_TO_TICKS(1500));
        bird_door_->Stop();
    }
    MotorPowerOff();
}

void CuckooStateMachine::BirdJumpOnce() {
    MotorPowerOn();
    if (bird_jump_) bird_jump_->Jump(300);
    // 脉冲结束后断电
    vTaskDelay(pdMS_TO_TICKS(400));
    MotorPowerOff();
}

void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
    MotorPowerOn();  // 舵机需持续供电保持位置
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
        case 2: if (m2_) m2_->SetSpeed(speed); break;
        case 3: if (m3_) m3_->SetSpeed(speed); break;
        case 4: if (m4_) m4_->SetSpeed(speed); break;
        default: break;
    }
    if (speed == 0) MotorPowerOff();
}

void CuckooStateMachine::SetBirdDoorSpeed(int speed) {
    if (speed != 0) MotorPowerOn();
    if (bird_door_) bird_door_->SetSpeed(speed);
    if (speed == 0) MotorPowerOff();
}

int CuckooStateMachine::PlayOnlineMusic(const char* url_or_path) {
    if (!mp3_) return -1;
    
    // Abort AI speech so music plays immediately at full volume
    // (otherwise AI keeps talking and music stays at 30% ducking)
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    
    // If already playing, stop old task cleanly to start new one.
    
    // Auto-detect format from URL path
    // 如果是完整URL（http开头），直接使用
    if (strncmp(url_or_path, "http", 4) == 0) {
        char conv_url[1280];
        strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
        conv_url[sizeof(conv_url) - 1] = '\0';
        ConvertToPcmUrl(conv_url, sizeof(conv_url));
        return mp3_->PlayOpus(conv_url);
    }
    
    // 否则用代理地址拼接
    if (music_proxy_host_.empty()) {
        ESP_LOGE(TAG, "Music proxy not configured. Check DEFAULT_MUSIC_PROXY_HOST in config.h.");
        return -10;
    }
    
    // URL编码非ASCII字符（中文等），esp_http_client不支持原始中文URL
    char encoded_path[1024];
    const char* src = url_or_path;
    char* dst = encoded_path;
    const char* end = encoded_path + sizeof(encoded_path) - 1;
    
    while (*src && dst < end) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x80) {
            // ASCII直接复制
            *dst++ = *src;
        } else {
            // 非ASCII → %XX 编码
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
    // Ensure path starts with /
    if (encoded_path[0] != '/') {
        snprintf(full_url, sizeof(full_url), "http://%s:%d/%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    } else {
        snprintf(full_url, sizeof(full_url), "http://%s:%d%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    }
    ConvertToPcmUrl(full_url, sizeof(full_url));
    return mp3_->PlayOpus(full_url);
}

void CuckooStateMachine::SetMusicProxy(const char* host, int port) {
    music_proxy_host_ = host;
    music_proxy_port_ = port;
    ESP_LOGI(TAG, "Music proxy set to %s:%d", host, port);
}

// ============================================
// CuckooTools MCP注册
// ============================================
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

void CuckooTools::RegisterAll() {
    auto& mcp = McpServer::GetInstance();

    // === Time / Chime ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.performance",
            "Hourly chime: bell rings + music. ONLY call when user explicitly says '报时/整点报时/几点/what time'. DO NOT auto-call on wake-up. For shows/singing/dancing use cuckoo.start_show instead.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int hour = props["hour"].value<int>();
                state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
                return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
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
        "Start performance show: dance + motors + music. For '表演/跳舞/播放音乐/节目'.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartShow();
            return std::string("{\"status\": \"show_started\"}");
        });

    mcp.AddTool("cuckoo.stop_all",
        "Stop motors, chime, performance. Does NOT stop music — use cuckoo.stop_music to stop music.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAll();
            return std::string("{\"status\": \"stopped\"}");
        });

    mcp.AddTool("cuckoo.stop_music",
        "Stop music playback. Call ONLY when user explicitly asks to stop the music (关歌/停音乐/不要放歌/别唱了). Do NOT call this for performance or alarm — use cuckoo.stop_all for those.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopMusic();
            return std::string("{\"status\": \"music_stopped\"}");
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

    mcp.AddTool("cuckoo.bird_jump",
        "Bird jump (electromagnet pulse).",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->BirdJumpOnce();
            return std::string("{\"status\": \"bird jumped\"}");
        });

    {
        PropertyList pl;
        pl.AddProperty(Property("servo_id", kPropertyTypeInteger, 0, 1));
        pl.AddProperty(Property("angle", kPropertyTypeInteger, 0, 180));
        mcp.AddTool("cuckoo.set_servo",
            "Set servo angle: 0=violinist, 1=dog.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int id = props["servo_id"].value<int>();
                int angle = props["angle"].value<int>();
                state_machine_->SetServoAngle(id, angle);
                return std::string("{\"status\": \"ok\"}");
            });
    }

    {
        PropertyList pl;
        pl.AddProperty(Property("motor_id", kPropertyTypeInteger, 1, 4));
        pl.AddProperty(Property("speed", kPropertyTypeInteger, -100, 100));
        mcp.AddTool("cuckoo.set_motor",
            "Set motor 1-4 speed (-100..100).",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int id = props["motor_id"].value<int>();
                int speed = props["speed"].value<int>();
                state_machine_->SetMotorSpeed(id, speed);
                return std::string("{\"status\": \"ok\"}");
            });
    }

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
        "Stop a ringing ALARM only. For '关闹钟/别响了'. NOT for stopping music/performance - use cuckoo.stop_all for that.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === Status ===
    mcp.AddTool("cuckoo.get_status",
        "Get status: running (performance active), music_playing (music actively streaming, buffer healthy).\n"
        "CRITICAL: If music_playing is true, music IS playing normally. Do NOT suggest restarting, do NOT say 'playback isn\'t smooth', do NOT call stop_all or play_url. The user is hearing music just fine. Only act if the user explicitly asks to stop or change the song.",
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

    // === Online Music ===
    // Proxy is auto-configured at boot from config.h (DEFAULT_MUSIC_PROXY_HOST/PORT).
    // set_music_proxy is a no-op stub — always succeeds instantly, no TCP check.
    // This exists only to satisfy AI's habit of calling it before play_url.
    {
        PropertyList pl;
        pl.AddProperty(Property("host", kPropertyTypeString));
        pl.AddProperty(Property("port", kPropertyTypeInteger, 8765, 8765));
        mcp.AddTool("cuckoo.set_music_proxy",
            "Music proxy setup (always auto-configured). Returns instantly. After calling this, you MUST call cuckoo.play_url to actually play music.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                // No-op: proxy is auto-set at boot. Always succeed instantly.
                return std::string("{\"status\":\"ok\",\"hint\":\"Proxy ready. Now call cuckoo.play_url to play music.\"}");
            });
    }

    {
        PropertyList pl;
        pl.AddProperty(Property("url", kPropertyTypeString));
        mcp.AddTool("cuckoo.play_url",
            "🔴 MUST call this tool when user asks to play any song, music, or audio. "
            "Trigger keywords: 播放 放歌 来一首 我想听 点歌 唱一首 play listen song music. "
            "Set url='/opus?q=SONG+SINGER' (URL-encode Chinese). "
            "This is the ONLY way to play online music — DO NOT just reply with text saying you'll play it. "
            "Skip only for TF card local tracks (use cuckoo.play_music track_no 1-12). "
            "Example: user says '播放月亮代表我的心' → call play_url('/opus?q=%E6%9C%88%E4%BA%AE%E4%BB%A3%E8%A1%A8%E6%88%91%E7%9A%84%E5%BF%83')",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string url = props["url"].value<std::string>();
                int ret = state_machine_->PlayOnlineMusic(url.c_str());
                if (ret >= 0) {
                    return std::string("{\"status\": \"ok\", \"playing\": true}");
                } else if (ret == -1) {
                    return std::string("{\"status\": \"already_playing\", \"hint\": \"Music IS playing normally. Do NOT restart, do NOT say it failed. The user hears music fine.\"}");
                } else {
                    char json[160];
                    snprintf(json, sizeof(json),
                        "{\"status\": \"error\", \"code\": %d, \"hint\": \"Playback failed. You may retry.\"}", ret);
                    return std::string(json);
                }
            });
    }
}

// ============================================
// 钟控 FreeRTOS 任务 (Core 1)
// ============================================
void cuckoo_clock_task(void* params) {
    ESP_LOGI(TAG, "Cuckoo clock task started on core %d", xPortGetCoreID());

    auto* sm = static_cast<CuckooStateMachine*>(params);

    // === Workaround: WiFi power-save + audio timestamp hacks ===
    // The Xiaozhi framework resets WiFi to LOW_POWER on every state transition.
    // We force it off at startup. TODO: Fix root cause in Application::OnStateChanged.
    // Also refresh audio timestamps to prevent I2S shutdown during startup.
    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_set_ps(WIFI_PS_NONE);  // HACK: fight framework's LOW_POWER
        as.RefreshOutputTimestamp();    // HACK: prevent audio watchdog timeout
        as.RefreshInputTimestamp();
        ESP_LOGI(TAG, "WiFi PM off + timestamp refresh (startup %d/3)", i + 1);
    }

    // 内部秒数计数器（不依赖真实 RTC，可通过 cuckoo.set_time MCP 工具校准）

    // 启动时尝试从 ESP32 系统时间（WiFi NTP）自动同步
    // 开机后 WiFi 已连接 NTP，gettimeofday 即可获取当前时间
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1000000000) {  // 2001年以后，说明 NTP 已同步
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

    // 设置默认音乐代理地址（来自 config.h，编译时固定）
    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);

    uint32_t tick_sec = 0;

    // TODO(#22): Replace 1s polling with event-driven approach:
    //   - Register OnDeviceStateChanged callback for idle/active transitions
    //   - Use a software timer for NTP sync instead of tick_sec counter
    //   - This would let Core 1 sleep most of the time, saving power
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tick_sec++;

        // 检测设备从idle唤醒（用户唤醒AI），记录时间戳用于防误触发
        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
            }
            sm->prev_device_state_ = dev_state;
        }

        // 每天同步一次 NTP 时间（86400秒），防止内部时钟漂移
        if (tick_sec % 86400 == 0) {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
                ESP_LOGI(TAG, "Daily NTP sync: %02d:%02d:%02d",
                         sm->current_hour_.load(), sm->current_min_.load(), sm->current_sec_.load());
            }
        }

        // 每 30 秒强制关 WiFi 省电（防止框架重新打开）
        if (tick_sec % 30 == 0) {
            esp_wifi_set_ps(WIFI_PS_NONE);
        }

        // 每 10 秒刷新音频芯片的活跃时间戳，防止电源管理超时关闭 I2S
        // （音频电源管理会在输入/输出空闲 15 秒后关闭 I2S，导致唤醒/喇叭失效）
        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();
        }

        // 如果时间已通过 MCP 设置，用内部时间
        if (sm->time_set_) {
            // 内部时钟每秒递增
            sm->current_sec_++;
            if (sm->current_sec_ >= 60) {
                sm->current_sec_ = 0;
                sm->current_min_++;
                if (sm->current_min_ >= 60) {
                    sm->current_min_ = 0;
                    sm->current_hour_ = (sm->current_hour_ + 1) % 24;
                }
            }

            // 整点和半点检查
            int h = sm->current_hour_;
            int m = sm->current_min_;

            // 整点报时（每分钟 == 0 时触发，用 NeedHourlyChime 防重复）
            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }
            // 半点报时（每分钟 == 30 时触发，用 NeedHalfHourlyChime 防重复）
            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }

            // 闹钟检查（每秒检查一次）
            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        // Note: NTP sync handled at startup + daily tick_sec % 86400 above
    }
}