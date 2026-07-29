// ============================================
// ӿ (Cuckoo Controller)
//
// ܸ
// - 4·ֱ + 1·񶯵 + ˮ
// - ƣСٶСβͶ
// - LED ˸· LED /
// - Ƶţ MP3/Opus/PCM 룬HTTP أDucking ܻ
// - /㱨ʱſ + СԾ + Ƚ + ֣
// - ۺϱݣ赸 + С + С + ˮ +  + LED
// - ɫ MCP ߣDogShowСLindaShowմGardenShow԰ӣ
// - 幦ܣNVS ־û5֧ÿظ
// - ҹģʽ22:00-6:00 
// - ֲţQQִOpus/PCM ʽأ
// - MCP עᣨ21ߣͨ MCP Э鹩 AI ģ͵ã
//
// мܹ
// - Core 0: ѴʡTTSOpus룩
// - Core 1: ӿcuckoo_clock_task250ms tickʱͱʱ
// - /ʱͨ xTaskCreatePinnedToCore  Core 1 ִ
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

// RTC ڴ棺/λ״̬´οԶʱ
RTC_NOINIT_ATTR struct {
    uint32_t magic;        // ħ 0xCAFEBABE ʾЧ
    uint32_t tick_sec;     // ǰ
    uint8_t  dev_state;    // 豸״̬
    uint8_t  music_active; // Ƿڲ
    uint32_t free_heap;    // ʣڴ
} rtc_crash_log;


//  /stream  /opus ʽURLתΪ /pcm ʽESP32Ƶʹ
static void ConvertToPcmUrl(char* url, size_t url_sz) {
    // 滻ַ
    size_t url_len = strlen(url);
    if (url_len >= url_sz) return;

    // /stream?q=... -> /pcm?q=...
    char* pos = strstr(url, "/stream?");
    if (pos) {
        // "/stream" = 7ַ, "/pcm" = 4ַ,  "?..."  pos+7 
        size_t tail = strlen(pos + 7) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 7, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // /opus?q=... -> /pcm?q=...
    pos = strstr(url, "/opus?");
    if (pos) {
        // "/opus" = 6ַ, "/pcm" = 4ַ,  "?..."  pos+5 
        size_t tail = strlen(pos + 5) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 5, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // ҲǰбܵAIʱ "stream?q= "  "opus?q= "
    pos = strstr(url, "stream?");
    if (pos && (pos == url || *(pos-1) != '/')) {
        // "stream" = 6ַ, "pcm" = 3ַ,  "?..."  pos+6 
        size_t tail = strlen(pos + 6) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 6, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
    pos = strstr(url, "opus?");
    if (pos && (pos == url || *(pos-1) != '/')) {
        // "opus" = 4ַ, "pcm" = 3ַ,  "?..."  pos+4 
        size_t tail = strlen(pos + 4) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 4, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
}
#define TAG "CuckooCtrl"

// ============================================
// LEDC PWM ͨ
// ============================================
// TB6612 : 4· PWM ͨ (Ƶ ~10-100KHz)
// : 2· PWM ͨ (Ƶ 50Hz)
// ŵ L9110S: 1· PWM ͨ (Ƶ ~1-10KHz)
// 7·ͨ

// ============================================
//  (TB6612 / DRV8833)
// ============================================

// GPIOʼ֧GPIOֱPWM˫ģʽ
static void motor_gpio_init(gpio_num_t in1, gpio_num_t in2) {
    gpio_config_t c = { .pin_bit_mask = (1ULL<<in1)|(1ULL<<in2),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}
// GPIOģʽ캯ֱPWM
Motor::Motor(gpio_num_t in1, gpio_num_t in2)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(false), max_duty_(0) { motor_gpio_init(in1, in2); Stop(); }
// PWMģʽ캯С (timer0, 1kHz/10-bit, dog-testһ)
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
// PWMģʽ캯Զ嶨ʱŵtimerָ
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
void Motor::Stop() {
    if (use_pwm_) { ledc_set_duty(speed_mode_, ledc_channel_, 0); ledc_set_duty(speed_mode_, ledc_channel2_, 0);
        ledc_update_duty(speed_mode_, ledc_channel_); ledc_update_duty(speed_mode_, ledc_channel2_); }
    else { gpio_set_level(in1_pin_, 0); gpio_set_level(in2_pin_, 0); }
}
void Motor::Forward(int speed) { SetSpeed(speed > 0 ? speed : 100); }
void Motor::Reverse(int speed) { SetSpeed(speed > 0 ? -speed : -100); }

// ============================================
//  (SG90)
// ============================================
Servo::Servo(gpio_num_t pin, ledc_channel_t channel, ledc_mode_t speed_mode)
    : pin_(pin), angle_(90), ledc_channel_(channel), speed_mode_(speed_mode) {

    // ʱȫֻʼһΣ50Hz, 14-bitֱʣ
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
 * @brief öǶ (0~180)
 * @param angle ĿǶȣԶǯλ [0, 180]
 * ǶתΪ50Hz PWMռձ (409~2048Ӧ 0~180)
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
 * @brief ƽɨ裺ʼǶȵĿǶȣָʱ
 * @param from ʼǶ
 * @param to ĿǶ  
 * @param duration_ms ʱ䣨룩ÿ20msһ
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

// С + ˮ (DRV8833  IN1/IN2)
// config.h : MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP
// ˮת: water_bird_->SetSpeed(WATER_WHEEL_SPEED)  IN1
// ˮת: water_bird_->SetSpeed(-100)  IN2

// ============================================
// MP3 (첽)
// ============================================
// Mp3Player - 벥첽棩
// ֧: DecodeSingleFile(MP3) / PlayUrl(HTTPMP3) / PlayPcm(ԭʼPCM) / PlayOpus(OGG/Opus)
// Ducking: AI˵ʱԶ100%20%Ϊ˵𽥻ָ100%
// л: LoadDogBarkPCMصڽѭе
// ============================================

/**
 * @brief ֹͣȴ˳5ڣ
 */
Mp3Player::~Mp3Player() {
    stop_requested_ = true;
    // 5˳recvرsocket
    for (int i = 0; i < 100 && is_playing_; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  // ܹ5
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

// ܣDucking  Ƶ·

// ܣDucking  Ƶ·
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
 * @brief عPCM
 * DecodeSingleFile/PlayUrlѭbark_active_
 * PCMӵϣʵֹкͬʱ
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
 * @brief 벢ŵMP3ļ
 * @param index MP3ļ (Ӧ 0001~0012.mp300130015մ0016԰)
 * @return 0=ֹͣ, 1=
 * - ID3v2ǩ룬OutputRawPcm
 * - лbark_active_Զӹ
 * - DuckingAI˵ʱ20%
 */
int Mp3Player::DecodeSingleFile(int index) {
    if (!assets_) {
        ESP_LOGE(TAG, "Assets not initialized");
        return 0;
    }

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

    //  assets ȡ MP3 ļ
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

    //  ID3v2 ǩMP3 ļ "ID3" ͷʱǰ10ֽں sync-safe int Ǳǩȣ
    uint8_t* mp3_start = (uint8_t*)mp3_data;
    size_t mp3_data_size = mp3_size;
    if (mp3_size > 10 && memcmp(mp3_start, "ID3", 3) == 0) {
        uint32_t id3_size = ((mp3_start[6] & 0x7F) << 21) | ((mp3_start[7] & 0x7F) << 14)
                          | ((mp3_start[8] & 0x7F) << 7)  |  (mp3_start[9] & 0x7F);
        size_t skip = 10 + id3_size;
        if (skip < mp3_size - 1024) {  // ȷ㹻
            mp3_start += skip;
            mp3_data_size -= skip;
            ESP_LOGI(TAG, "Skipped ID3v2 tag: %lu bytes", (unsigned long)skip);
        }
    }

    // رս
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return 0;
    }

        // Ϣ
    esp_audio_dec_info_t dec_info = {};
    bool info_ready = false;
    int sample_rate = 22050;
    int channels = 1;

        // ѭ
    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 50;

    while (remaining > 0 && !stop_requested_) {
        // ׼֡
        size_t in_len = (remaining < kInputBufSize) ? remaining : kInputBufSize;
        memcpy(input_buf_, input_ptr, in_len);

        esp_audio_dec_in_raw_t raw;
        raw.buffer = input_buf_;
        raw.len = in_len;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        // ׼֡
        esp_audio_dec_out_frame_t frame;
        frame.buffer = output_buf_;
        frame.len = kOutputBufSize;
        frame.needed_size = 0;
        frame.decoded_size = 0;

        // 
        ret = esp_mp3_dec_decode(mp3_dec_handle_, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK) {
            // ״λȡϢ  esp_mp3_dec_decode ͨ dec_info 
            if (!info_ready && frame.decoded_size > 0) {
                sample_rate = dec_info.sample_rate ? dec_info.sample_rate : 22050;
                channels = dec_info.channel ? dec_info.channel : 1;
                ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %.1f kbps",
                         sample_rate, channels, dec_info.bitrate / 1000.0f);
                info_ready = true;
            }

            // н PCM ݣƸͨ
            if (frame.decoded_size > 0 && !stop_requested_) {
                int16_t* pcm_data = (int16_t*)frame.buffer;
                size_t num_samples = frame.decoded_size / sizeof(int16_t);

                auto& app = Application::GetInstance();

 // ---- Smooth ducking: fade music out when AI starts speaking ----
                // Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring
                static int duck_state = 0;  // 0=idle, 1=fading, 2=ducked, 3=restoring
                static int64_t duck_transition_us = 0;
                bool ai_speaking = (app.GetDeviceState() == kDeviceStateSpeaking);

                if (disable_ducking_) {
                    // ģʽܣȫ
                    ducking_gain_ = 1.0f;
                } else if (ai_speaking && duck_state == 0) {
                    // ʼ
                    duck_state = 1;
                    duck_transition_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "DecodeSingleFile: AI speaking, fading out");
                }

                if (duck_state == 1) {
                    // 400ms20%
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ <= 0.2f) {
                        ducking_gain_ = 0.2f;
                        duck_state = 2;
                    }
                    if (!ai_speaking) {
                     // AIڽڼֹͣ  ָ
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
                    // 400msָ100%
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 0.2f + 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ >= 1.0f) {
                        ducking_gain_ = 1.0f;
                        duck_state = 0;
                        ESP_LOGI(TAG, "DecodeSingleFile: false trigger, restored");
                    }
                    if (ai_speaking) {
                     // AIڻָڼֿʼ˵  ½
                        duck_state = 1;
                        duck_transition_us = esp_timer_get_time();
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

                // йݴȥ
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

            // ȴɣOutputRawPcm 
                int play_ms = (int)(num_samples * 1000 / sample_rate / (channels > 0 ? channels : 1));
                if (play_ms < 10) play_ms = 10;
                vTaskDelay(pdMS_TO_TICKS(play_ms));
            }

            // ƶָ
            size_t consumed = raw.consumed;
            if (consumed == 0) {
                consumed = 1;  //  Tiny files advance byte-by-byte
                if (consumed > remaining) consumed = remaining;
            }
        consecutive_errors = 0;  // ɹô
            input_ptr += consumed;
            remaining -= consumed;

        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer too small, needed %lu", (unsigned long)frame.needed_size);
            break;
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            ESP_LOGE(TAG, "Unsupported MP3 format (stopping)");
            break;
        } else {
        // ǰ֡
            consecutive_errors++;
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "Too many consecutive decode errors (%d), aborting", consecutive_errors);
                break;
            }
            ESP_LOGW(TAG, "Decode error %d, skipping frame", ret);
            size_t skip = raw.consumed ? raw.consumed : 1;
            if (skip > remaining) skip = remaining;
            input_ptr += skip;
            remaining -= skip;
        }
    }

    // رս
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    // ָСǵ Opus Ǳ϶ָ
    auto& app = Application::GetInstance();
    if (stop_requested_) {
        app.GetAudioService().FlushOutputDma();
    }
    app.GetAudioService().SetOutputMuted(false);

    ESP_LOGI(TAG, "Finished playing %s (stopped=%d)", filename, stop_requested_.load() ? 1 : 0);
    return !stop_requested_;
}

// ============================================
    //   3.  JSON Ӧ (ȡ url ֶκ)

// ============================================
int Mp3Player::PlayUrl(const char* url) {
    if (!url || url[0] == '\0') return -1;

    ESP_LOGI(TAG, "PlayUrl: launching background task for %s", url);

    // ̨
    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PlayUrlCtx();
    ctx->self = this;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->url[sizeof(ctx->url) - 1] = '\0';

    xTaskCreate(PlayUrlTask, "mp3_url", 8192, ctx, 5, NULL);
        return 0;  // 
}

/**
 * @brief ͨHTTPMP3ţ̨첽
 * @param url HTTP URL
 * - أ4MBϸMPEG֡ͷУ飬+ز24000Hz
 * - DuckingAI˵ʱԶ͵20%
 * - ʽ룺һһѭֱ򱻴
 */
    // ϸ MPEG ֡ͷУ鲻 FF Ex У bitrate/samplerate/layer
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

    // HTTP ͻ
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

        // 
    void* dec_handle = nullptr;
    esp_audio_err_t dec_ret = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PlayUrl: decoder fail %d", dec_ret);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }

    self->is_playing_ = true;
    app.GetAudioService().SetOutputMuted(true);

    // 壺Content-Length֪Σ߽֡λᣩ
    size_t batch_size = 256 * 1024;
    if (content_length > 0 && content_length < 8 * 1024 * 1024) {
        batch_size = content_length; // ȫһ
        if (batch_size > 4 * 1024 * 1024) batch_size = 4 * 1024 * 1024; // 4MB
        ESP_LOGI(TAG, "PlayUrl: batch=%dKB (content=%dKB)", (int)(batch_size/1024), content_length/1024);
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        // PSRAMʧܡ˵256KB
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

    // ز + תã
    const int kOutRate = 24000;
                // Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz
 // =1152, ز=24000/8000=3.0, =3456
 // 4096ȫϲ
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
    // һ + ԤID3ǩ + 
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

    // ----  ID3v2 ǩ ----
    mp3_start = 0;
    if (batch_len > 10 && memcmp(buf, "ID3", 3) == 0) {
        uint32_t id3_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14)
                          | ((buf[8] & 0x7F) << 7)  |  (buf[9] & 0x7F);
        mp3_start = 10 + id3_size;
        if (mp3_start > batch_len - 1024) mp3_start = 0; // ǩ̫/ݲ㣬ͷʼ
        ESP_LOGI(TAG, "PlayUrl: ID3v2 %luB, MP3 @ %u", id3_size, (unsigned)mp3_start);
    }

    // ---- ԭʼɨ MPEG ֡ͷȡ ----
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

    // ---- ѭʽ----
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

 //  + ز24000Hz
 // OutputRawPcmڲزᵼң
                    if (sample_rate != kOutRate && mono_ns > 0) {
                    // Step 1: stereo -> mono (average L/R)
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }
 // 2ԲֵزkOutRate
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
 // ӦDucking
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
 // ԻӦDucking
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
        // ýص frame_size  raw.consumed ȷ߽֡羫ȷ
                size_t c = dec_info.frame_size;
                if (c == 0) c = raw.consumed;  // ˻
                if (c == 0) c = 1;
                if (c >= rem) { rem = 0; } else { ptr += c; rem -= c; }
            } else if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (raw.consumed > 0 && raw.consumed < rem) { ptr += raw.consumed; rem -= raw.consumed; }
                else if (raw.consumed == 0) {
                    // ʼ֡ͷһ MPEG ֹͬ֡ѭ
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 < rem) { ptr += scan; rem -= scan; }
                    else { rem = 0; break; }  // δҵͬ֡ͷcarryʣ
                }
                if (rem < 1024) break;
            } else {
                if (raw.consumed > 0) {
                    if (raw.consumed >= rem) { rem = 0; }
                    else { ptr += raw.consumed; rem -= raw.consumed; }
                } else {
                    // ʧδѡϸһ MPEG ֡ͷ
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 >= rem) { rem = 0; break; }
                    ptr += scan; rem -= scan;
                }
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d rem=%u stop=%d", batch_seq, (unsigned)rem, self->stop_requested_.load() ? 1 : 0);
        if (self->stop_requested_) { printf("DD stop_break\n"); break; }

        // ----- ĩβδֽڣƴӵͷ -----
        size_t carry = rem;
        if (carry > 0 && carry < batch_size) {
            memmove(buf, ptr, carry);
        } else {
            carry = 0;
        }

        // ----- һ -----
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
        // carry=0ҷ֡ͷɨmetadata/֡ݵһϸ֡ͷ
        if (carry == 0 && rem > 3 && !IsValidMpegHeader(ptr)) {
            size_t s = 1;
            while (s + 3 < rem && !IsValidMpegHeader(ptr + s)) s++;
            if (s + 3 < rem) {
                ESP_LOGD(TAG, "PlayUrl: skip %d bytes to next frame", (int)s);
                ptr += s; rem -= s;
            } else {
                rem = 0; // Ч֡ͷ
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

// PlayPcm: HTTPԭʼPCM  PushRawPcmToPlaybackԭŹߣ֡⣩
/**
 * @brief ͨHTTPԭʼs16le PCM͵Ŷ
 * @param url PCMƵURL
 * @return 0=ɹ
 * - ֿأ4KBPushRawPcmToPlayback벥Ŷ
 * - DuckingAI˵ʱԶ20%
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

 // ֿPCM벥Ŷ
    const size_t CHUNK = sizeof(self->output_buf_);  // С
    const int SAMPLE_RATE = 24000;
    int64_t t0 = esp_timer_get_time();
    int bytes_yielded = 0;

    while (self->is_playing_ && !self->stop_requested_) {
        int read = esp_http_client_read(client, (char*)self->output_buf_, CHUNK);
        if (read <= 0) break;

        size_t samples = read / 2;  // 16λ
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
                self->ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
                if (self->ducking_gain_ < 0.2f) {
                    self->ducking_gain_ = 0.2f;  // hold at 20% as background
                    ESP_LOGI(TAG, "PlayPcm: fade complete, holding at 20%%");
                }
            }
        }
 // ԻӦDucking
        float g = self->ducking_gain_;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = (int32_t)(pcm[i] * g);
            if (s > 30000) s = 30000;
            if (s < -30000) s = -30000;
            pcm[i] = (int16_t)s;
        }

        app.GetAudioService().PushRawPcmToPlayback(pcm, samples, SAMPLE_RATE);

 // ÿԼ4ƵóCPUֹWiFi
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
 * @brief ͨԭʼBSD SocketOGG/OpusƵ͸
 * @param url OpusƵURL
 * @return 0=ɹ
 * - ʹBSD socketֱƹlwip esp_http_client
 * - Opus·OGGװ  PushPacketToDecodeQueue(ԭOpus)
 * - PCM·legacyPushBackgroundAudio(߱Ƶring buffer)
 * - DuckingAI˵ʱͣݣݣ˵ָ
 * - Զ͵65%AECزɸţ
 * - 粻ͨʱ˵תģʽ
 */
