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

// RTC 涓嶆帀鐢靛唴瀛橈細姝绘満/澶嶄綅鍚庝繚鐣欑姸鎬侊紝涓嬫寮€鏈鸿嚜鍔ㄦ姤
RTC_NOINIT_ATTR struct {
    uint32_t magic;        // 魔锟斤拷 0xCAFEBABE 锟斤拷示锟斤拷锟斤拷锟斤拷效
    uint32_t tick_sec;     // Initialize timer锟斤拷锟斤拷锟斤拷锟�
    uint8_t  dev_state;    // 锟斤拷锟斤拷璞缸刺�
    uint8_t  music_active; // 锟斤拷锟斤拷锟角凤拷锟节诧拷
    uint32_t free_heap;    // 剩锟斤拷锟斤拷诖锟�
} rtc_crash_log;


// Helper: convert server /stream or /opus URL to /pcm URL for ESP32 background audio
static void ConvertToPcmUrl(char* url, size_t url_sz) {
 // All replacements shrink the string.
    size_t url_len = strlen(url);
    if (url_len >= url_sz) return;

    // /stream?q=... -> /pcm?q=...
    char* pos = strstr(url, "/stream?");
    if (pos) {
        // "/stream" = 7 chars, "/pcm" = 4 chars, keep "?..." at pos+7
        size_t tail = strlen(pos + 7) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 7, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
    // /opus?q=... -> /pcm?q=...
    pos = strstr(url, "/opus?");
    if (pos) {
        // "/opus" = 6 chars, "/pcm" = 4 chars, keep "?..." at pos+5
        size_t tail = strlen(pos + 5) + 1;
        if ((size_t)(pos + 4 + tail - url) > url_sz) return;
        memmove(pos + 4, pos + 5, tail);
        memcpy(pos, "/pcm", 4);
        return;
    }
 // Also handle without leading slash (AI sometimes sends "stream?q= " or "opus?q= ")
    pos = strstr(url, "stream?");
    if (pos && (pos == url || *(pos-1) != '/')) {
        // "stream" = 6 chars, "pcm" = 3 chars, keep "?..." at pos+6
        size_t tail = strlen(pos + 6) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 6, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
    pos = strstr(url, "opus?");
    if (pos && (pos == url || *(pos-1) != '/')) {
        // "opus" = 4 chars, "pcm" = 3 chars, keep "?..." at pos+4
        size_t tail = strlen(pos + 4) + 1;
        if ((size_t)(pos + 3 + tail - url) > url_sz) return;
        memmove(pos + 3, pos + 4, tail);
        memcpy(pos, "pcm", 3);
        return;
    }
}
#define TAG "CuckooCtrl"

// ============================================
// LEDC PWM channel allocation
// ============================================
// TB6612 requires 4 PWM channels
// Servo requires 2 PWM channels
// Bird door motor: 1 PWM channel
// Total 7 channels // TB6612 PWM requires ~10-100KHz
// Servo requires 50Hz
// Bird door motor L9110S requires ~1-10KHz

// ============================================
// Motor (TB6612 / DRV8833)
// ============================================

// Motor: GPIO direct drive / PWM dual-mode
static void motor_gpio_init(gpio_num_t in1, gpio_num_t in2) {
    gpio_config_t c = { .pin_bit_mask = (1ULL<<in1)|(1ULL<<in2),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}
// GPIO mode constructor
Motor::Motor(gpio_num_t in1, gpio_num_t in2)
    : in1_pin_(in1), in2_pin_(in2), use_pwm_(false), max_duty_(0) { motor_gpio_init(in1, in2); Stop(); }
// PWM mode Dog motor (timer0, 1kHz/10-bit, matches dog-test)
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
// PWM mode Custom timer (bird door)
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
// Servo (SG90)
// ============================================
Servo::Servo(gpio_num_t pin, ledc_channel_t channel, ledc_mode_t speed_mode)
    : pin_(pin), angle_(90), ledc_channel_(channel), speed_mode_(speed_mode) {

 // LEDC_TIMER_SERVO only config once
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

void Servo::SetAngle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    angle_ = angle;
    uint32_t duty = (uint32_t)(409 + (float)(angle) / 180.0f * (2048 - 409));
    ledc_set_duty(speed_mode_, ledc_channel_, duty);
    ledc_update_duty(speed_mode_, ledc_channel_);
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

// BirdJump + Motor (DRV8833 IN1/IN2 )
// config.h: MOTOR_WATER_BIRD_IN1/IN2 + LEDC_CH_WATER/JUMP
// : water_bird_->SetSpeed(WATER_WHEEL_SPEED) IN1=HIGH
// : water_bird_->SetSpeed(-100) IN2=HIGH

// ============================================
// MP3 Player (YX5200/YX5300 UART)
// ============================================
// ============================================
// Mp3Player - 杞欢瑙ｇ爜鎾斁鍣紙寮傛浠诲姟鐗堬級
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

// MP3 

// Shared ducking methods (used by all audio playback paths)
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

int Mp3Player::DecodeSingleFile(int index) {
    if (!assets_) {
        ESP_LOGE(TAG, "Assets not initialized");
        return 0;
    }

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

    // 浠� assets 鑾峰彇 MP3 鏂囦欢鏁版嵁
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

    // 璺宠繃 ID3v2 鏍囩锛圡P3 鏂囦欢浠� "ID3" 寮€澶存椂锛屽墠 10 瀛楄妭鍚庣殑 sync-safe int 鏄爣绛鹃暱搴︼級
    uint8_t* mp3_start = (uint8_t*)mp3_data;
    size_t mp3_data_size = mp3_size;
    if (mp3_size > 10 && memcmp(mp3_start, "ID3", 3) == 0) {
        uint32_t id3_size = ((mp3_start[6] & 0x7F) << 21) | ((mp3_start[7] & 0x7F) << 14)
                          | ((mp3_start[8] & 0x7F) << 7)  |  (mp3_start[9] & 0x7F);
        size_t skip = 10 + id3_size;
        if (skip < mp3_size - 1024) {  // 纭繚璺宠繃鍚庤繕鏈夎冻澶熸暟鎹�
            mp3_start += skip;
            mp3_data_size -= skip;
            ESP_LOGI(TAG, "Skipped ID3v2 tag: %lu bytes", (unsigned long)skip);
        }
    }

    // 鍏抽棴瑙ｇ爜鍣�
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return 0;
    }

    // 瑙ｇ爜淇℃伅
    esp_audio_dec_info_t dec_info = {};
    bool info_ready = false;
    int sample_rate = 22050;
    int channels = 1;

    // 瑙ｇ爜寰幆
    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 50;

    while (remaining > 0 && !stop_requested_) {
        // 鍑嗗杈撳叆甯�
        size_t in_len = (remaining < kInputBufSize) ? remaining : kInputBufSize;
        memcpy(input_buf_, input_ptr, in_len);

        esp_audio_dec_in_raw_t raw;
        raw.buffer = input_buf_;
        raw.len = in_len;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        // 鍑嗗杈撳嚭甯�
        esp_audio_dec_out_frame_t frame;
        frame.buffer = output_buf_;
        frame.len = kOutputBufSize;
        frame.needed_size = 0;
        frame.decoded_size = 0;

        // 瑙ｇ爜
        ret = esp_mp3_dec_decode(mp3_dec_handle_, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK) {
            // 棣栨鑾峰彇瑙ｇ爜淇℃伅 鈥� esp_mp3_dec_decode 宸查€氳繃 dec_info 鍙傛暟濉厖
            if (!info_ready && frame.decoded_size > 0) {
                sample_rate = dec_info.sample_rate ? dec_info.sample_rate : 22050;
                channels = dec_info.channel ? dec_info.channel : 1;
                ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %.1f kbps",
                         sample_rate, channels, dec_info.bitrate / 1000.0f);
                info_ready = true;
            }

            // 鏈夎В鐮佸嚭鐨� PCM 鏁版嵁锛屾帹缁欐贩闊抽€氶亾
            if (frame.decoded_size > 0 && !stop_requested_) {
                int16_t* pcm_data = (int16_t*)frame.buffer;
                size_t num_samples = frame.decoded_size / sizeof(int16_t);

                auto& app = Application::GetInstance();

 // ---- Smooth ducking: fade music out when AI starts speaking ----
                // Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring
                static int duck_state = 0;  // 0=idle, 1=fading, 2=ducked, 3=restoring
                static int64_t duck_transition_us = 0;
                bool ai_speaking = (app.GetDeviceState() == kDeviceStateSpeaking);

                if (ai_speaking && duck_state == 0) {
                    // Start fading out
                    duck_state = 1;
                    duck_transition_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "DecodeSingleFile: AI speaking, fading out");
                }

                if (duck_state == 1) {
                    // Fading out over 400ms to 20%
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ <= 0.2f) {
                        ducking_gain_ = 0.2f;
                        duck_state = 2;
                    }
                    if (!ai_speaking) {
                        // AI stopped during fade 鈥� restore
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
                    // Restoring over 400ms to 100%
                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 0.2f + 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ >= 1.0f) {
                        ducking_gain_ = 1.0f;
                        duck_state = 0;
                        ESP_LOGI(TAG, "DecodeSingleFile: false trigger, restored");
                    }
                    if (ai_speaking) {
                        // AI started again during restore
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

                // 杞欢娣烽煶锛氬鏋滄湁鐙楀彨鏁版嵁寰呮挱锛屽彔鍔犱笂鍘�
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

                // 绛夊緟鎾斁瀹屾垚锛圤utputRawPcm 闈為樆濉烇級
                int play_ms = (int)(num_samples * 1000 / sample_rate / (channels > 0 ? channels : 1));
                if (play_ms < 10) play_ms = 10;
                vTaskDelay(pdMS_TO_TICKS(play_ms));
            }

            // 绉诲姩杈撳叆鎸囬拡
            size_t consumed = raw.consumed;
            if (consumed == 0) {
                consumed = 1;  //  Tiny files advance byte-by-byte
                if (consumed > remaining) consumed = remaining;
            }
            consecutive_errors = 0;  // 瑙ｇ爜鎴愬姛锛岄噸缃敊璇鏁�
            input_ptr += consumed;
            remaining -= consumed;

        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer too small, needed %lu", (unsigned long)frame.needed_size);
            break;
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            ESP_LOGE(TAG, "Unsupported MP3 format (stopping)");
            break;
        } else {
            // 瑙ｇ爜閿欒锛氳烦杩囧綋鍓嶅抚
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

    // 鍏抽棴瑙ｇ爜鍣�
    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    // 鎭㈠灏忔櫤鐨� Opus 瑙ｇ爜杈撳嚭
    auto& app = Application::GetInstance();
    if (stop_requested_) {
        app.GetAudioService().FlushOutputDma();
    }
    app.GetAudioService().SetOutputMuted(false);

    ESP_LOGI(TAG, "Finished playing %s (stopped=%d)", filename, stop_requested_.load() ? 1 : 0);
    return !stop_requested_;
}

// ============================================
//   3. 浠ｇ悊鏈嶅姟 JSON 鍝嶅簲 (鎻愬彇 url 瀛楁鍚庨噸璇�)
// PlayUrl - HTTP涓嬭浇闊抽骞舵挱鏀�
// PlayUrl - HTTP涓嬭浇闊抽骞舵挱鏀�
//   3. 浠ｇ悊鏈嶅姟 JSON 鍝嶅簲 (鎻愬彇 url 瀛楁鍚庨噸璇�)
//   3. 浠ｇ悊鏈嶅姟 JSON 鍝嶅簲 (鎻愬彇 url 瀛楁鍚庨噸璇�)
// ============================================
int Mp3Player::PlayUrl(const char* url) {
    if (!url || url[0] == '\0') return -1;

    ESP_LOGI(TAG, "PlayUrl: launching background task for %s", url);

    // 鍚庡彴浠诲姟涓婁笅鏂�
    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PlayUrlCtx();
    ctx->self = this;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->url[sizeof(ctx->url) - 1] = '\0';

    xTaskCreate(PlayUrlTask, "mp3_url", 8192, ctx, 5, NULL);
    return 0;  // Initialize timer锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷
}

// 涓ユ牸 MPEG 甯уご鏍￠獙涓嶄粎妫€鏌� FF Ex杩樻牎楠� bitrate/samplerate/layer
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

    // HTTP 瀹㈡埛绔�
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

    // 瑙ｇ爜鍣�
    void* dec_handle = nullptr;
    esp_audio_err_t dec_ret = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PlayUrl: decoder fail %d", dec_ret);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }

    self->is_playing_ = true;
    app.GetAudioService().SetOutputMuted(true);

    // 鎵归噺缂撳啿锛氬鏋淐ontent-Length宸茬煡锛岀敤灏戦噺澶ф壒娆★紙闄嶄綆甯ц竟鐣岄敊浣嶆満浼氾級
    size_t batch_size = 256 * 1024;
    if (content_length > 0 && content_length < 8 * 1024 * 1024) {
        batch_size = content_length; // 全锟斤拷锟斤拷锟斤拷
        if (batch_size > 4 * 1024 * 1024) batch_size = 4 * 1024 * 1024; // Initialize timer锟斤拷4MB
        ESP_LOGI(TAG, "PlayUrl: batch=%dKB (content=%dKB)", (int)(batch_size/1024), content_length/1024);
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        // PSRAM鍒嗛厤澶辫触鈫掑洖閫€鍒�256KB鍐嶈瘯
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

    // 閲嶉噰鏍风紦鍐插尯锛堢珛浣撳０鈫掑崟澹伴亾 + 閲囨牱鐜囪浆鎹㈢敤锛�
    const int kOutRate = 24000;
                // Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz
 // mono_ns = 1152, ratio_max = 24000/8000 = 3.0, out_ns_max = 3456
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
 // 绗竴鎵逛笅杞� + 棰勫鐞嗚烦杩嘔D3鏍囩 + 瑙ｆ瀽閲囨牱鐜�
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

 // ---- 璺宠繃 ID3v2 鏍囩 ----
    mp3_start = 0;
    if (batch_len > 10 && memcmp(buf, "ID3", 3) == 0) {
        uint32_t id3_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14)
                          | ((buf[8] & 0x7F) << 7)  |  (buf[9] & 0x7F);
        mp3_start = 10 + id3_size;
        if (mp3_start > batch_len - 1024) mp3_start = 0; // 锟斤拷签太锟斤拷/锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷
        ESP_LOGI(TAG, "PlayUrl: ID3v2 %luB, MP3 @ %u", id3_size, (unsigned)mp3_start);
    }

    // ---- 浠庡師濮嬫暟鎹壂鎻� MPEG 甯уご鑾峰彇閲囨牱鐜� ----
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

    // ---- 瑙ｇ爜寰幆锛堟壒閲忔祦寮忥級----
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

 // Mix stereomono + resample to 24000Hz if needed
 // OutputRawPcm's internal resampler treats stereo as mono (cross-channel garbling)
                    if (sample_rate != kOutRate && mono_ns > 0) {
                    // Step 1: stereo -> mono (average L/R)
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }
 // Step 2: linear resamplekOutRate
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
 // 鐢ㄨВ鐮佸櫒杩斿洖鐨� frame_size 浠ｆ浛 raw.consumed 纭繚甯ц竟鐣岀簿纭�
                size_t c = dec_info.frame_size;
                if (c == 0) c = raw.consumed;  // 锟斤拷锟斤拷
                if (c == 0) c = 1;
                if (c >= rem) { rem = 0; } else { ptr += c; rem -= c; }
            } else if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (raw.consumed > 0 && raw.consumed < rem) { ptr += raw.consumed; rem -= raw.consumed; }
                else if (raw.consumed == 0) {
                    // 鏁版嵁璧峰涓嶅湪甯уご锛岃烦杩囪嚦涓嬩竴涓� MPEG 鍚屾甯э紙闃叉姝诲惊鐜級
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 < rem) { ptr += scan; rem -= scan; }
                    else { rem = 0; break; }  // 未锟揭碉拷同锟斤拷帧锟斤拷锟斤拷锟斤拷锟斤拷 carry 锟斤拷锟斤拷一锟斤拷
                }
                if (rem < 1024) break;
            } else {
                if (raw.consumed > 0) {
                    if (raw.consumed >= rem) { rem = 0; }
                    else { ptr += raw.consumed; rem -= raw.consumed; }
                } else {
                    // 瑙ｇ爜澶辫触涓旀湭娑堣垂鈫掍弗鏍艰烦鑷充笅涓€涓� MPEG 甯уご
                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 >= rem) { rem = 0; break; }
                    ptr += scan; rem -= scan;
                }
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d rem=%u stop=%d", batch_seq, (unsigned)rem, self->stop_requested_.load() ? 1 : 0);
        if (self->stop_requested_) { printf("DD stop_break\n"); break; }

        // ----- 淇濈暀涓婃壒鏈熬鏈秷璐瑰瓧鑺傦紝鎷兼帴鍒颁笅鎵瑰ご閮� -----
        size_t carry = rem;
        if (carry > 0 && carry < batch_size) {
            memmove(buf, ptr, carry);
        } else {
            carry = 0;
        }

        // ----- 涓嬭浇涓嬩竴鎵� -----
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
        // carry=0涓旈潪甯уご鈫掓壂鎻忚烦杩噈etadata/闈炲抚鏁版嵁鍒颁笅涓€涓弗鏍煎抚澶�
        if (carry == 0 && rem > 3 && !IsValidMpegHeader(ptr)) {
            size_t s = 1;
            while (s + 3 < rem && !IsValidMpegHeader(ptr + s)) s++;
            if (s + 3 < rem) {
                ESP_LOGD(TAG, "PlayUrl: skip %d bytes to next frame", (int)s);
                ptr += s; rem -= s;
            } else {
                rem = 0; // Initialize timer锟斤拷效帧头锟斤拷锟斤拷锟斤拷
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

// 鈹€鈹€ PlayPcm: HTTP涓嬭浇raw PCM 鈫� PushRawPcmToPlayback锛堣蛋鍘熺敓鎾斁绠＄嚎锛屾棤甯у榻愰棶棰橈級鈹€鈹€
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
    int bytes_yielded = 0;

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
                self->ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
                if (self->ducking_gain_ < 0.2f) {
                    self->ducking_gain_ = 0.2f;  // hold at 20% as background
                    ESP_LOGI(TAG, "PlayPcm: fade complete, holding at 20%%");
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

// volume: 0.0~1.0锛屾帶鍒堕煶閲忥紙娓愬己鐢級
// volume: 0.0~1.0锛屾帶鍒堕煶閲忥紙娓愬己鐢級
// HTTP stream OGG/Opus, feed to OggDemuxer -> PushPacketToDecodeQueue (native pipeline)
int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        ESP_LOGW(TAG, "PlayOpus: music already playing, refusing (call stop_music first)");
        return -1;
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
    stop_requested_ = false;  // 锟斤拷锟斤拷洗锟� Stop() 锟斤拷锟斤拷锟侥憋拷锟�
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

 // Music uses background audio layer let ducking handle AI interaction.

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
            if (app.GetDeviceState() == kDeviceStateConnecting) break;  // wake word 锟斤拷 stop
 // Double-trigger with 500ms gap to avoid false stop from music false-wake
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
 // Same pipeline as AI voice Opus decoder playback queue output
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
        
 // Lower volume during music to reduce AEC bleed VAD works better
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
 // Downloaded data is discarded music skips forward (barely noticeable
 // for a 3C5 min song during a 5C15 s AI response).
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
    
 // ============ PCM path (legacy): raw s16le PushBackgroundAudio ============
    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    int64_t t0 = esp_timer_get_time(), last_refresh = t0;
    size_t total_dl = 0;
    bool ai_speaking = false;

    app.GetAudioService().SetBackgroundAudioGain(0.001f);  // near-silent but keeps bg_audio_active_=true
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
 // Set gain based on current AI state before enabling drain
 // (prevents full-volume music + TTS overlap noise)
            auto state = app.GetDeviceState();
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.3f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGI(TAG, "PlayOpus: drain enabled (buffered %d samples in %d ms)",
                     (int)fill, (int)((esp_timer_get_time() - prebuf_start) / 1000));
            break;
        }
        if (esp_timer_get_time() - prebuf_start > 10000000) {
 // Set gain even on timeout otherwise stays at 0.001f
            auto state = app.GetDeviceState();
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.3f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGW(TAG, "PlayOpus: pre-buffer timeout after 10s (%d samples buffered)",
                     (int)fill);
            break;
        }
    }

 // === Main download push loop ===
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
 // UDP audio packets (prevents WiFi buffer starvation TTS stutter).
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
 // Wait 1s before retry WiFi may be reconnecting after AI conversation
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

    // If device is idle, kick wake word detection to ensure mic input is alive.
    // After long background audio playback, the audio input path may have stalled
    // (esp_codec_dev_read returns stale/zero data without error), making the
    // wake word detector deaf even though it's nominally "running".
    if (app.GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "PlayOpus: restarting wake word detection after music");
        app.GetAudioService().EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        app.GetAudioService().EnableWakeWordDetection(true);
    }

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

 // Generate and play in 100ms chunks stop_requested_ checked between chunks
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

// 鎾斁浠诲姟鍏ュ彛
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
        // 閽熷０
        PlayBell(track);
    } else if (folder == 2) {
        // 闊充箰
        if (track < 1) track = 1;
        if (track > 12) track = 12;
        PlayIndex(track);
    }
}

void Mp3Player::PlayIndex(uint16_t index) {
    if (index < 1) index = 1;
    if (index > 16) index = 16;

    if (!assets_) {
        ESP_LOGE(TAG, "Mp3Player not initialized (call Init first)");
        return;
    }

    // 鍋滄褰撳墠鎾斁
    Stop();

    // 娓呯┖ stop_requested_ 骞惰缃柊鏇茬洰
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;

    // 纭繚浠诲姟宸插垱寤�
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

    // 鍋滄褰撳墠鎾斁
    Stop();

    // 璁剧疆閽熷０閲嶅娆℃暟
    pending_bell_hour_ = hour;

    // 纭繚浠诲姟宸插垱寤�
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
    if (index > 16) index = 16;
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
    if (max_samples > 2 * 1024 * 1024) max_samples = 2 * 1024 * 1024;  // cap at 2M samples (4MB)
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

    // 璺宠繃 ID3v2 鏍囩
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
// LDR light sensor (ADC oneshot mode)
// ============================================
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

int LdrSensor::ReadRaw() {
    int raw = 0;
    if (adc_handle_) {
        adc_oneshot_read(adc_handle_, adc_chan_, &raw);
    }
    return raw;  // 0-4095 (12-bit), bright=high, dark=low
}

bool LdrSensor::IsDark() { return ReadRaw() < threshold_; }
void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }


// ============================================
// BellSoundPlayer - 閫氳繃 AI 闊抽绯荤粺鎾斁甯冭胺楦熷彨?// ============================================
void BellSoundPlayer::PlayCuckooSoundSync() {
    // 涓嶉渶瑕� SetOutputMuted锛屼篃涓嶉渶瑕� vTaskDelay
    // 涓嶉渶瑕� SetOutputMuted锛屼篃涓嶉渶瑕� vTaskDelay
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_wake_sound,
        CUCKOO_WAKE_SOUND_NUM_SAMPLES,
        CUCKOO_WAKE_SOUND_SAMPLE_RATE
    );
}

void BellSoundPlayer::PlayBellSoundSync() {
 // 鐩存帴鍐� I2S闃诲鐩村埌鎾畬浣跨敤 data_if_mutex_ 涓� AudioOutputTask 浜掓枼
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
                                        Motor* violin_motor, Servo* violin, Servo* dog,
                                        Motor* water_bird, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
      violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),  // 灏忔彁鐞寸數鏈� (鏉緽 M2, GPIO18/45)
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
 // 鐢垫満鐢垫簮P-MOSFET鎺у埗 (GPIO LOW=ON, HIGH=OFF)
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);
 // LED (GPIO1KS8050 B, C, 5V)
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_A_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    MotorPowerOff();  // 默锟较断碉拷, 100K锟斤拷锟斤拷锟斤拷5V
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

// ============================================
// 鍏辩敤鑸炶箞搴忓垪锛圥erformanceTask + ShowTask 璋冪敤锛�
// ============================================

// 寮傛寮€闂ㄤ换鍔″弬鏁�
struct DoorOpenCtx {
    CuckooStateMachine* sm;
};

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

void CuckooStateMachine::PlayDogBark() {
    // 浠� assets 璇诲彇 dog_bark.wav (16kHz mono s16)
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

    // 鎶婄嫍鍙� PCM 鍔犺浇鍒� Mp3Player锛孌ecodeSingleFile 鐨勮緭鍑哄惊鐜細鑷姩娣烽煶
    const int16_t* pcm = (const int16_t*)(wav_data + 44);
    size_t pcm_bytes = wav_size - 44;
    size_t num_samples = pcm_bytes / sizeof(int16_t);

    if (mp3_) {
        mp3_->LoadDogBark(pcm, num_samples);
    }
}

void CuckooStateMachine::RunDanceIntro() {
    // 寮傛寮€闂紙涓嶉樆濉烇紝涓庤垶韫�/闊充箰骞惰锛�
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open", 2048, ctx, 5, nullptr, 1);

    // 灏忕嫍鑸垫満 180->20锛堜笌寮€闂ㄥ苟琛岋級
    if (dog_servo_) {
        dog_servo_->Sweep(180, 20, (180 - 20) * 15);
    }
    // 灏忕嫍鍓嶈繘 鈫� 鍋� 鈫� 鍙紙鍜岃儗鏅煶涔愬悓鏃舵挱锛�
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(920));
        m3_->Stop();
    }
    PlayDogBark();
    // 灏忕嫍鑸垫満 20->0
    if (dog_servo_) {
        dog_servo_->Sweep(20, 0, 20 * 15);
    }
}

void CuckooStateMachine::RunDanceLoop() {
    unsigned long m1_timer = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    int64_t next_frame_us = esp_timer_get_time();

    while (is_running_ && mp3_->IsPlaying()) {
        if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // M1 鑸炶箞鐢垫満: 姝ｈ浆/鍙嶈浆浜ゆ浛
        switch (m1_stage) {
            case 0:
                if (m1_) m1_->Forward(DANCE_SPEED_PERCENT);
                if (now - m1_timer > m1_rand_time) { m1_timer = now; m1_stage = 1; }
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
                if (now - m1_timer > m1_rand_time) { m1_timer = now; m1_stage = 3; }
                break;
            case 3:
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }

        // 灏忔彁鐞村惊鐜姩浣滐細姣忓抚 9掳 骞虫粦鎵姩
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

        // 灏忕嫍鎽囧熬宸�
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

        // LED 闂儊 (姣� 6 甯� ~300ms)
        led_toggle++;
        if (led_toggle >= 6) {
            led_toggle = 0;
            led_state = !led_state;
            gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
        }

        // 50ms 甯х巼鎺у埗
        next_frame_us += 50000;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
        }
    }
}

void CuckooStateMachine::RunDanceFinale() {
    // 闊充箰缁撴潫锛屼袱涓茬伅鐝犱竴榻愪寒锛堢洿鍒板ぇ闂ㄥ叧闂悗鍦� PerformanceTask/ShowTask 鏈熬鐔勭伃锛�
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);

    // 鍋滆垶韫堢數鏈�
    if (m1_) m1_->Stop();

    // 骞宠　灏忔彁鐞存鍙嶈浆娆℃暟
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

    // 灏忕嫍閫€鍥�
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(920));
        m3_->Stop();
    }
    // 灏忕嫍鑸垫満褰掍綅 0->180
    if (dog_servo_) {
        dog_servo_->Sweep(0, 180, 180 * 15);
    }
    // 鍏抽棬
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
}

// ============================================
// NVS 闂归挓鎸佷箙鍖�
// ============================================

static const char* kAlarmNvsNamespace = "cuckoo_alarm";
static const char* kAlarmNvsKey = "alarms";

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
    // 缁熻鏈夋晥闂归挓鏁�
    int count = 0;
    for (int i = 0; i < kMaxAlarms; i++) {
        if (alarms_[i].enabled) count++;
    }
    alarm_count_ = count;
    ESP_LOGI(TAG, "Alarms loaded from NVS (%d active)", count);
}


void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();

 // 闃睞I璇Е鍙戣澶囧垰浠巌dle鍞ら啋5绉掑唴璺宠繃 performanceAI鍙兘鍦ㄥ敜閱掓椂鑷姩璋僣uckoo.performance
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {  // 5锟诫窗锟斤拷
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }

 // 濡傛灉 AI 姝ｅ湪璇磋瘽/鐩戝惉鍏堟墦鏂畠
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
        vTaskDelay(pdMS_TO_TICKS(200));  // 锟斤拷状态锟斤拷锟叫伙拷锟斤拷 idle
    }
 // 琛ㄦ紨鏈熼棿绂佹帀璇煶澶勭悊闃叉閽熷０琚璁や负鍞ら啋璇嶅敜閱� AI
    app.GetAudioService().EnableVoiceProcessing(false);
 // 鏄惧紡绂佹帀鍞ら啋璇嶆娴嬫棫鐗堟湰 EnableVoiceProcessing 鍙兘涓嶅鐞嗘閫昏緫
    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
            total_calls_ = hour;       // 锟斤拷录锟角硷拷锟姐（锟斤拷锟斤拷锟矫ｏ拷
            call_count_ = 3;           // 锟教讹拷锟斤拷锟斤拷锟斤拷
            break;
        case kPerformanceHalf:
            total_calls_ = 0;
            call_count_ = 3;           // 锟斤拷锟揭诧拷潭锟斤拷锟斤拷锟斤拷锟�
            break;
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
        default: is_running_ = false; return;
    }
 // Initialize timer Core 1 
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


void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Performance task started");

 // 闈欓煶 AI 杈撳嚭娓呯┖鎾斁闃熷垪闃叉閽熷０鍜� Opus 娣峰彔
    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    // ====== 鏁寸偣鎶ユ椂绠€鍖栫増锛堢數鏈烘棤瀹夎锛屽彧璧伴煶棰戞祦绋嬶級 ======
    if (sm->current_performance_ == kPerformanceHour) {
        // Phase 1: 楦熼棬鎵撳紑 鈫� 灏忛笩璺冲嚭+鍙� 鈫� 楦熼棬鍏抽棴
        sm->OpenBirdDoor();  // 楦熼棬 (m4_)
        for (int i = 0; i < sm->call_count_; i++) {
            sm->BirdJumpPulse();  // 灏忛笩鑴夊啿璺冲嚭
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundSync();
            }
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        sm->CloseBirdDoor();  // 楦熼棬鍏抽棴

        // 涓€娆℃€цВ鐮佸埌PSRAM锛岀敤OutputRawPcm閲嶅鎾斁锛岃烦杩嘙P3瑙ｇ爜鍣ㄥ弽澶嶆墦寮€鐨勬姈鍔�
        // 涓€娆℃€цВ鐮佸埌PSRAM锛岀敤OutputRawPcm閲嶅鎾斁锛岃烦杩嘙P3瑙ｇ爜鍣ㄥ弽澶嶆墦寮€鐨勬姈鍔�
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            int16_t* bell_pcm = nullptr;
            size_t bell_samples = 0;
            int bell_sr = 0;
            int dec_ret = sm->mp3_->DecodeToBuffer(13, &bell_pcm, &bell_samples, &bell_sr);
            if (dec_ret == 0 && bell_pcm && bell_samples > 0) {
                ESP_LOGI(TAG, "Bell PCM loaded: %zu samples @ %d Hz", bell_samples, bell_sr);
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    app.GetAudioService().OutputRawPcm(bell_pcm, bell_samples, bell_sr);
                    // OutputRawPcm 鏄樆濉炵殑锛屾挱瀹屾墠杩斿洖
                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                heap_caps_free(bell_pcm);
            } else {
 // 鍥為€€鐢� PlayIndex
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


 // Phase 3: ++++璧�++
        app.GetAudioService().SetOutputMuted(false);  // 锟街革拷AI锟斤拷锟酵�
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // 水锟斤拷

 // Phase 2: bell chime - 瑙ｇ爜0013.mp3鍒癙CM缂撳啿鍖哄惊鐜挱閬垮厤MP3瑙ｇ爜鍣ㄦ姈鍔�
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            sm->mp3_->PlayBgMusic(sm->total_calls_);
            // 绛夐煶涔愬紑濮嬫挱鏀撅紙鏈€澶氱瓑3绉掞級
            int wait_start = 0;
            while (wait_start < 6 && sm->is_running_ && !sm->mp3_->IsPlaying()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                wait_start++;
            }

            sm->violin_state_.fwd_count = 0;
            sm->violin_state_.rev_count = 0;
            sm->RunDanceIntro();
            sm->RunDanceLoop();
            sm->RunDanceFinale();
        }

    // Stop water wheel + LEDs off
        if (sm->water_bird_) sm->water_bird_->Stop();
        gpio_set_level(LED_A_GPIO, 0);
        gpio_set_level(LED_B_GPIO, 0);

    // ====== 鍗婄偣鎶ユ椂锛氶笩闂�+灏忛笩璺冲嚭+鍙竴澹� ======
    } else if (sm->current_performance_ == kPerformanceHalf) {
        sm->OpenBirdDoor();  // 楦熼棬鎵撳紑
        for (int i = 0; i < sm->call_count_; i++) {
            sm->BirdJumpPulse();  // 灏忛笩鑴夊啿璺冲嚭
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundSync();
            }
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        sm->CloseBirdDoor();  // 楦熼棬鍏抽棴
 // 闈欓煶鎭㈠ AI
        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== Done ======
    if (sm->mp3_) sm->mp3_->Stop();
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    sm->current_phase_ = kPhaseIdle;
    sm->MotorPowerOff();
    // 鎭㈠鍞ら啋璇嶆娴嬶紙涓嶈皟 EnableVoiceProcessing 鈥� 閬垮厤 ResetDecoder 骞叉壈 TTS锛�
    // 鍙湪 idle 鐘舵€佹墠鎭㈠锛岄伩鍏嶅湪 listening 鐘舵€佽瑙﹀彂鍞ら啋璇�
    if (app.GetDeviceState() == kDeviceStateIdle) {
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    is_dark_ = dark;
    if (is_running_) return;

 // AI 蹇欐椂鎴栬儗鏅煶棰戞挱鏀句腑涓嶆姤鏃�
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle || Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        ESP_LOGI(TAG, "Skipping chime: device busy (state=%d, bg_audio=%d)", state,
                 (int)Application::GetInstance().GetAudioService().IsBgAudioActive());
        return;
    }

    // 澶滈棿妯″紡锛�20:00-6:00锛変笉鎶ユ椂
    if ((is_dark_ && hour >= 20) || hour < 6) {
        return;
    }

    // 鏁寸偣鎶ユ椂
    // 鏁寸偣鎶ユ椂

    // 鏁寸偣鎶ユ椂
    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting performance", hour, min);
        StartPerformance(kPerformanceHour, hour);
        MarkHourlyChime(hour);  // dedup after successful trigger
    }
    // 鍗婄偣鎶ユ椂
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
// 闂归挓鍔熻兘
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
    SaveAlarmsToNvs();
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
 // First round: ramp volume 15%100% over first 10 calls (20s)
        for (int i = 0; i < 50; i++) {
            if (sm->alarm_stopped_ || !sm->alarm_ringing_) break;
            if (sm->mp3_) {
                float volume;
                if (i < 10) {
                    volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% 锟斤拷 100% over first 10
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

// ============================================
//掕壊琛ㄦ紨
// ============================================

void CuckooStateMachine::DogShow() {
    if (is_running_) return;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);
    }, "dog_show", 4096, this, 5, nullptr, 1);
}

void CuckooStateMachine::DogShowTask() {
    auto& app = Application::GetInstance();
    auto st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking || st == kDeviceStateListening || st == kDeviceStateConnecting) {
        ESP_LOGI(TAG, "DogShow: aborting AI (state=%d)", (int)st);
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 30 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    app.GetAudioService().SetOutputMuted(true);

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");

    MotorPowerOn();

    // 姘磋溅杞姩
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 寮€闂�
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 灏忕嫍璺戝嚭鏉�
    if (dog_servo_) dog_servo_->Sweep(60, 0, 60 * DOG_TAIL_STEP_MS);
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }

    // 姹豹鍙竴澹�
    PlayDogBarkDirect();

    // 鎽囧ご鏅冭剳 10 绉�
    unsigned long start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long end_time = start + 10000;
    int sweep_dir = 0;  // 0: 0->60, 1: 60->0
    while (is_running_ && (xTaskGetTickCount() * portTICK_PERIOD_MS) < end_time) {
        if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);
        if (dog_servo_) {
            if (sweep_dir == 0) {
                dog_servo_->Sweep(0, 60, 60 * DOG_TAIL_STEP_MS);
                vTaskDelay(pdMS_TO_TICKS(500));
                sweep_dir = 1;
            } else {
                dog_servo_->Sweep(60, 0, 60 * DOG_TAIL_STEP_MS);
                vTaskDelay(pdMS_TO_TICKS(500));
                sweep_dir = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 鍐嶆豹姹彨涓€澹�
    PlayDogBarkDirect();

    // 灏忕嫍閫€鍥�
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }
    if (dog_servo_) dog_servo_->Sweep(60, 0, 60 * DOG_TAIL_STEP_MS);

    // 灏忕嫍閫€鍥炲悗鎵嶅仠姘磋溅
    if (water_bird_) water_bird_->Stop();

    // 鍏抽棬
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    // 鎭㈠璇煶
    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    app.GetAudioService().EnableVoiceProcessing(true);
    app.GetAudioService().EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "DogShow: done");
}

void CuckooStateMachine::PlayDogBarkDirect() {
    // 浠� assets 璇诲彇 dog_bark.wav 骞剁洿鎺ョ敤 OutputRawPcm 鎾斁
    const char* filename = "dog_bark.wav";
    void* wav_ptr = nullptr;
    size_t wav_size = 0;
    if (!Assets::GetInstance().GetAssetData(filename, wav_ptr, wav_size)) {
        ESP_LOGW(TAG, "PlayDogBarkDirect: %s not found", filename);
        return;
    }
    if (wav_size < 44) return;

    const uint8_t* wav_data = (const uint8_t*)wav_ptr;
    uint16_t channels = wav_data[22] | (wav_data[23] << 8);
    uint32_t sample_rate = wav_data[24] | (wav_data[25] << 8) | (wav_data[26] << 16) | (wav_data[27] << 24);
    uint16_t bits = wav_data[34] | (wav_data[35] << 8);
    if (bits != 16 || channels != 1) {
        ESP_LOGW(TAG, "PlayDogBarkDirect: need mono s16");
        return;
    }

    const int16_t* pcm = (const int16_t*)(wav_data + 44);
    size_t num_samples = (wav_size - 44) / sizeof(int16_t);
    ESP_LOGI(TAG, "DogBarkDirect: %u samples, %d Hz", (unsigned)num_samples, sample_rate);

    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(pcm, num_samples, sample_rate);
    // 绛夊緟鎾斁瀹屾垚
    int play_ms = (int)(num_samples * 1000 / sample_rate);
    vTaskDelay(pdMS_TO_TICKS(play_ms + 100));
}

void CuckooStateMachine::StartLindaShow() {
    if (is_running_) return;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->LindaShow();
        vTaskDelete(nullptr);
    }, "linda_show", 4096, this, 5, nullptr, 1);
}

void CuckooStateMachine::StartGardenShow() {
    if (is_running_) return;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->GardenShow();
        vTaskDelete(nullptr);
    }, "garden_show", 4096, this, 5, nullptr, 1);
}

void CuckooStateMachine::LindaShow() {
    auto& app = Application::GetInstance();
    auto st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking || st == kDeviceStateListening || st == kDeviceStateConnecting) {
        ESP_LOGI(TAG, "LindaShow: aborting AI (state=%d)", (int)st);
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 30 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    // 绛� TTS-STOP 澶勭悊瀹屾瘯锛圓bortSpeaking 瑙﹀彂鐨� TTS-STOP 鍙兘寤惰繜锛岄噸鏂扮鐢ㄨ闊冲鐞嗭級
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    app.GetAudioService().SetOutputMuted(true);

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");

    // 1. 涓ED涓€璧蜂寒
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. 闊充箰鍝嶈捣
    if (mp3_) mp3_->PlayBgMusic(15);

    // 绛夐煶涔愬紑濮�
    int wait = 0;
    while (wait < 6 && is_running_ && !mp3_->IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED闂儊 + 鑸炶箞锛岀洿鑷抽煶涔愮粨鏉�
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long m1_timer = music_start_ms;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;

    while (is_running_ && mp3_->IsPlaying()) {
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        // 闊充箰鍋滄鍓�2绉掞紙绾�26-2=24绉掞級锛孡ED涓€璧蜂寒锛屽仠姝㈤棯鐑�
        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

        // 鑸炶箞鐢垫満
        switch (m1_stage) {
            case 0:
                if (m1_) m1_->Forward(DANCE_SPEED_PERCENT);
                if (now - m1_timer > m1_rand_time) { m1_timer = now; m1_stage = 1; }
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
                if (now - m1_timer > m1_rand_time) { m1_timer = now; m1_stage = 3; }
                break;
            case 3:
                if (m1_) m1_->Stop();
                if (now - m1_timer > 20) {
                    m1_timer = now; m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }
        // LED浜ゆ浛闂儊锛堜粎鍦ㄥ墠24绉掞級
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

    // 5. 闊充箰缁撴潫 鈫� 鑸炶箞鍋� 鈫� LED鐏�
    if (m1_) m1_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    if (m1_) m1_->Stop();
    if (mp3_) mp3_->Stop();
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    app.GetAudioService().EnableVoiceProcessing(true);
    app.GetAudioService().EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "LindaShow: done");
}

void CuckooStateMachine::GardenShow() {
    auto& app = Application::GetInstance();
    auto st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking || st == kDeviceStateListening || st == kDeviceStateConnecting) {
        ESP_LOGI(TAG, "GardenShow: aborting AI (state=%d)", (int)st);
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 30 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    // 绛� TTS-STOP 澶勭悊瀹屾瘯锛圓bortSpeaking 瑙﹀彂鐨� TTS-STOP 鍙兘寤惰繜锛岄噸鏂扮鐢ㄨ闊冲鐞嗭級
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    app.GetAudioService().SetOutputMuted(true);

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");

    // 1. 涓ED涓€璧蜂寒
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. 闊充箰鍝嶈捣
    if (mp3_) mp3_->PlayBgMusic(16);

    // 绛夐煶涔愬紑濮�
    int wait = 0;
    while (wait < 6 && is_running_ && !mp3_->IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED闂儊 + 灏忔彁鐞磋埖鏈猴紝鐩磋嚦闊充箰缁撴潫
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int violin_angle = SERVO_CENTER_ANGLE;
    int violin_dir = 0;  // 0: decreasing, 1: increasing
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;

    while (is_running_ && mp3_->IsPlaying()) {
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

        // Non-blocking servo: move 9 degrees per 50ms frame
        if (violin_servo_) {
            if (violin_dir == 0) {
                violin_angle -= 9;
                if (violin_angle <= 0) { violin_angle = 0; violin_dir = 1; }
            } else {
                violin_angle += 9;
                if (violin_angle >= 180) { violin_angle = 180; violin_dir = 0; }
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

    // 5. 闊充箰缁撴潫 鈫� 鑸炶箞鍋� 鈫� LED鐏�
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    if (mp3_) mp3_->Stop();
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    app.GetAudioService().EnableVoiceProcessing(true);
    app.GetAudioService().EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "GardenShow: done");
}

void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Dance start (Arduino-style)");

    // M1 鑸炶箞鐢垫満: 姝ｈ浆闅忔満1-3绉� 鈫� 鍋� 鈫� 鍙嶈浆闅忔満1-3绉� 鈫� 寰幆
    // M1 鑸炶箞鐢垫満: 姝ｈ浆闅忔満1-3绉� 鈫� 鍋� 鈫� 鍙嶈浆闅忔満1-3绉� 鈫� 寰幆
    // M1 鑸炶箞鐢垫満: 姝ｈ浆闅忔満1-3绉� 鈫� 鍋� 鈫� 鍙嶈浆闅忔満1-3绉� 鈫� 寰幆
    unsigned long start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long end = start + 8000; // 锟斤拷时锟斤拷8锟斤拷

    int m1_stage = 0;
    unsigned long m1_timer = start;
    int m1_rand_time = 1000 + (esp_random() % 2001); // 1-3锟斤拷

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS) < end) {
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // M1 鑸炶箞鐢垫満: 姝ｈ浆/鍙嶈浆浜ゆ浛
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

 // : (board B M2 via violin_motor_)
        if (violin_motor_) violin_motor_->Forward(VIOLIN_WARMUP_PERCENT);

        // 灏忔彁鐞磋埖鏈�: 90鈫�0鈫�180鈫�90 寰幆鎽嗗姩
        if (violin_servo_) violin_servo_->Sweep(45, 135, 1500);

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (m1_) m1_->Stop();
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    is_running_ = false;
    MotorPowerOff();
    ESP_LOGI(TAG, "Dance finished");
}


void CuckooStateMachine::StartShow() {
 // 鏁寸偣/鍗婄偣鎶ユ椂浠诲姟姝ｅ湪杩愯 涓嶅厑璁� Show 鎵撴柇AI 璇皟淇濇姢
    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }
    // 鍏朵粬鎯呭喌锛堝闂归挓锛夊彲浠ュ厛鍋滃啀寮€濮�
    if (is_running_) {
        StopAll();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // 鎭㈠灏忔櫤鐨� Opus 瑙ｇ爜杈撳嚭锛堟棤璁烘甯哥粨鏉熻繕鏄鎵撴柇閮藉繀椤绘仮澶嶏級
    auto& app = Application::GetInstance();
    auto st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking || st == kDeviceStateListening || st == kDeviceStateConnecting) {
        ESP_LOGI(TAG, "Aborting AI before show (state=%d)", (int)st);
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 30 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // 琛ㄦ紨鏈熼棿绂佹帀璇煶澶勭悊鍜屽敜閱掕瘝妫€娴嬶紝闃叉闊充箰琚璁ゅ敜閱掕瘝
    // 瀵艰嚧闊抽閫氳矾鍐茬獊 鈫� esp_codec_dev_write panic
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);

    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

 // MCP
 // Core 1 5 = mp3_play FreeRTOS 
    xTaskCreatePinnedToCore(
        ShowTask,
        "cuckoo_show",
        4096,
        this,
        5,
        nullptr,
        1
    );
}

void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Show task started");

    // 绛� TTS-STOP 澶勭悊瀹屾瘯锛圓bortSpeaking 瑙﹀彂鐨� TTS-STOP 鍙兘寤惰繜3绉�
    // 灏嗙姸鎬� speaking鈫抣istening 骞堕噸鏂板惎鐢� voice processing锛岃鐩� StartShow 閲岀殑绂佺敤锛�
    vTaskDelay(pdMS_TO_TICKS(500));
    auto& app = Application::GetInstance();
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(false);
    app.GetAudioService().SetOutputMuted(true);
    ESP_LOGI(TAG, "ShowTask: voice processing re-disabled after TTS-STOP settle");

    // ====== Show start: LEDs on + water wheel ======
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // 水锟斤拷 IN1=HIGH

    // 閫夐煶涔�
    int next = (esp_random() % 12) + 1;
    if (next == sm->show_music_index_ && next < 12) {
        next++;
    } else if (next == sm->show_music_index_) {
        next = 1;
    }
    sm->show_music_index_ = next;
    if (sm->mp3_) sm->mp3_->PlayBgMusic(next);

    // 绛夐煶涔愬紑濮嬫挱鏀撅紙鏈€澶氱瓑3绉掞級
    int wait_start = 0;
    while (wait_start < 6 && sm->is_running_ && !sm->mp3_->IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_start++;
    }

    sm->violin_state_.fwd_count = 0;
    sm->violin_state_.rev_count = 0;
    ESP_LOGI(TAG, "DOG-MOTOR: m3_=%p", sm->m3_);

    // 寮傛鍚姩寮€闂�+灏忕嫍鍑哄満锛屼笉闃诲鑸炶箞寰幆
    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);

    // 鑸炶箞+灏忔彁鐞寸珛鍒诲紑濮嬶紙鍜屽紑闂�/灏忕嫍鍑哄満骞惰锛�
    sm->RunDanceLoop();

    // 闊充箰缁撴潫锛屾仮澶嶅敜閱掕瘝鍜岃闊冲鐞�
    // 娉ㄦ剰锛歋tartShow 鐨� AbortSpeaking 鍙兘璁╃姸鎬佸崱鍦� listening锛堥潪 idle锛夛紝
    // 鐩存帴鎭㈠璇煶澶勭悊浼氬鑷村敜閱掕瘝鍦� listening 鐘舵€佽Е鍙� AbortSpeaking 鈫� audio_input 姝婚攣
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    app.GetAudioService().SetOutputMuted(false);

    // 纭繚璁惧鍥炲埌 idle 鍐嶆仮澶嶈闊冲鐞�
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    app.GetAudioService().EnableVoiceProcessing(true);
    app.GetAudioService().EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "Show: music ended, wake word + voice processing restored");

    sm->RunDanceFinale();

    // ====== Stop water wheel + LEDs off ======
    if (sm->water_bird_) sm->water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    sm->MotorPowerOff();

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
    if (m1_) m1_->Stop();  // 锟借蹈
    if (m2_) m2_->Stop();  // 锟斤拷锟斤拷
    if (m3_) m3_->Stop();  // 小锟斤拷
    if (m4_) m4_->Stop();  // 锟斤拷锟斤拷
    if (violin_motor_) violin_motor_->Stop();  // 小锟斤拷锟斤拷
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    // 璋冪敤 stop_all+play_url 杩炵幆閲嶆斁鎵撴柇闊充箰
    // 璋冪敤 stop_all+play_url 杩炵幆閲嶆斁鎵撴柇闊充箰
    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
            mp3_->Stop();
        }
    }

 // 涓嶇洿鎺� AbortSpeaking TTS 鍙兘姝ｅ湪姝ｅ父鎾斁璁╁畠鑷劧缁撴潫
 // 涓嶇洿鎺� AbortSpeaking TTS 鍙兘姝ｅ湪姝ｅ父鎾斁璁╁畠鑷劧缁撴潫
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
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
    MotorPowerOff();
}

void CuckooStateMachine::CloseDoor() {
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
    MotorPowerOff();
}

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

void CuckooStateMachine::BirdJumpShort() {
    MotorPowerOn();
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    int pulse = 50 + (esp_random() % 151);  // random 50-200ms
    vTaskDelay(pdMS_TO_TICKS(pulse));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    int cooldown = 300 + (esp_random() % 401);  // random 300-700ms
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

void CuckooStateMachine::OpenBirdDoor() {
    MotorPowerOn();
    if (m4_) {
        m4_->Forward(BIRD_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
    MotorPowerOff();
}

void CuckooStateMachine::CloseBirdDoor() {
    MotorPowerOn();
    if (m4_) {
        m4_->Reverse(BIRD_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
    MotorPowerOff();
}

void CuckooStateMachine::PlayCuckooSound() {
    MotorPowerOn();
    if (bell_player_) {
        bell_player_->PlayCuckooSoundSync();
    }
    MotorPowerOff();
}

void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
    MotorPowerOn();  // Initialize timer锟斤拷锟斤拷锟界保锟斤拷位锟斤拷
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
        case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;  // 灏忔彁鐞寸數鏈� (鏉緽 M2, GPIO18/45)
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

int CuckooStateMachine::PlayOnlineMusic(const char* url_or_path) {
    if (!mp3_) return -1;
    
 // Abort AI speech to prevent CPU overload from excessively long
 // TTS responses (can reach 100-200+ Opus packets). Without this,
 // the AFE ring buffer overflows watchdog reset.
 // The MCP response is already sent before this, so the AI knows
    // playback was started successfully.
    auto& app = Application::GetInstance();
    auto ai_state = app.GetDeviceState();
    if (ai_state == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
        ESP_LOGI(TAG, "PlayOnlineMusic: aborting AI speech to play music");
    }
    
 // If already playing, stop old task cleanly to start new one.
    
    // Auto-detect format from URL path
    // 濡傛灉鏄畬鏁碪RL锛坔ttp寮€澶达級锛岀洿鎺ヤ娇鐢�
    if (strncmp(url_or_path, "http", 4) == 0) {
        char conv_url[1280];
        strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
        conv_url[sizeof(conv_url) - 1] = '\0';
        // 缂栫爜 URL 涓殑绌烘牸涓� %20
        for (char* p = conv_url; *p; p++) {
            if (*p == ' ') {
                // 鍚庣Щ 2 瀛楄妭
                size_t tail_len = strlen(p + 1);
                if ((size_t)(p + 3 + tail_len - conv_url) >= sizeof(conv_url)) break;
                memmove(p + 3, p + 1, tail_len + 1);
                p[0] = '%'; p[1] = '2'; p[2] = '0';
            }
        }
        ConvertToPcmUrl(conv_url, sizeof(conv_url));
        // 濡傛灉姝ｅ湪鎾斁锛屽厛鍋滄棫姝屽啀鏀炬柊姝�
        if (mp3_->IsPlaying()) {
            ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
            mp3_->Stop();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        return mp3_->PlayOpus(conv_url);
    }
    
    // 鍚﹀垯鐢ㄤ唬鐞嗗湴鍧€鎷兼帴
    if (music_proxy_host_.empty()) {
        ESP_LOGE(TAG, "Music proxy not configured. Check DEFAULT_MUSIC_PROXY_HOST in config.h.");
        return -10;
    }
    
 // URL-encode non-ASCII chars (Chinese etc.), esp_http_client does not support raw Chinese URLs
    char encoded_path[1024];
    const char* src = url_or_path;
    char* dst = encoded_path;
    const char* end = encoded_path + sizeof(encoded_path) - 1;
    
    while (*src && dst < end) {
        unsigned char c = (unsigned char)*src;
        if (c == ' ') {
            // 绌烘牸鍦� HTTP request line 涓槸鍒嗛殧绗︼紝蹇呴』缂栫爜
            if (dst + 3 <= end) {
                *dst++ = '%';
                *dst++ = '2';
                *dst++ = '0';
            }
            src++;
            continue;
        }
        if (c < 0x80) {
 // ASCII鐩存帴澶嶅埗
            *dst++ = *src;
        } else {
 // 闈濧SCII %XX 缂栫爜
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
    // 濡傛灉姝ｅ湪鎾斁锛屽厛鍋滄棫姝屽啀鏀炬柊姝�
    if (mp3_->IsPlaying()) {
        ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
        mp3_->Stop();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return mp3_->PlayOpus(full_url);
}

void CuckooStateMachine::SetMusicProxy(const char* host, int port) {
    music_proxy_host_ = host;
    music_proxy_port_ = port;
    ESP_LOGI(TAG, "Music proxy set to %s:%d", host, port);
}

// ============================================
// CuckooTools MCP娉ㄥ唽
// ============================================
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

void CuckooTools::RegisterAll() {
    auto& mcp = McpServer::GetInstance();

 // === Time / Chime ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.performance",
            "Hourly chime: bell rings + music. ONLY call when user explicitly says '锟斤拷时/锟斤拷锟姐报时/锟斤拷锟斤拷/what time'. DO NOT auto-call on wake-up. For shows/singing/dancing use cuckoo.start_show instead.",
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
        "Start performance show: dance + motors + music. For '锟斤拷锟斤拷/锟斤拷锟斤拷/锟斤拷锟斤拷锟斤拷锟斤拷/锟斤拷目'.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartShow();
            return std::string("{\"status\": \"show_started\"}");
        });

    mcp.AddTool("cuckoo.stop_all",
        "Stop motors, chime, performance. Does NOT stop music 锟斤拷 use cuckoo.stop_music to stop music.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAll();
            return std::string("{\"status\": \"stopped\"}");
        });

    mcp.AddTool("cuckoo.stop_music",
        "Stop music playback. Call ONLY when user explicitly asks to stop the music (锟截革拷/停锟斤拷锟斤拷/锟斤拷要锟脚革拷/锟斤拷锟斤拷). Do NOT call this for performance or alarm 锟斤拷 use cuckoo.stop_all for those.",
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
        "Stop a ringing ALARM only. For '锟斤拷锟斤拷锟斤拷/锟斤拷锟斤拷锟斤拷'. NOT for stopping music/performance - use cuckoo.stop_all for that.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === Status ===
    mcp.AddTool("cuckoo.get_status",
        "Get status: running (performance active), music_playing (music actively streaming, buffer healthy).\n"
        "CRITICAL: If music_playing is true, music IS playing normally. Do NOT suggest restarting or say playback failed. "
        "If user asks for a DIFFERENT song, call cuckoo.play_url with the new song - it will auto-stop the current one. "
        "If user just says stop, call cuckoo.stop_music. Only call stop_all if user wants everything to stop.",
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
 // set_music_proxy is a no-op stub always succeeds instantly, no TCP check.
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
            "Play online music. When user says a song name, call this IMMEDIATELY without asking. "
            "Set url='/pcm?q=SONG_NAME' (URL-encode Chinese, encode spaces as %20). "
            "If music is already playing and user asks for a DIFFERENT song, this tool will auto-stop the old song and play the new one. "
            "Do NOT ask 'do you want to change?' - just call play_url directly. "
            "Do NOT reply 'I will play it' - call the tool, do not just say it. "
            "Examples: user says 'play jiu gan tang mai wu' -> call play_url('/pcm?q=%E9%85%92%E5%B9%B2%E5%80%98%E5%8D%96%E6%97%A0'). "
            "For local tracks use cuckoo.play_music (track 1-12) instead.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string url = props["url"].value<std::string>();
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
    // === 瑙掕壊琛ㄦ紨 ===
    mcp.AddTool("cuckoo.dog_show",
        "灏忕嫍鍑哄満琛ㄦ紨锛氬紑闂ㄣ€佸皬鐙楄窇鍑恒€佹豹姹彨涓ゅ０銆佹憞澶存檭鑴�20绉掋€佸啀鍙袱澹般€侀€€鍥炪€佸叧闂ㄣ€�"
        "褰撶敤鎴疯 '灏忕嫍' '灏忕嫍鍛�' '灏忕嫍灏忕嫍' '涓借帋' '涓借帋鍛�' '涓借帋鍦ㄥ摢閲�' 鏃惰皟鐢ㄦ宸ュ叿銆�"
        "Dog show: call when user asks about dog or Lisa. Keep response very brief - one short sentence only.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->DogShow();
            return std::string("{\"status\": \"dog_show_started\"}");
        });

    mcp.AddTool("cuckoo.linda_show",
        "鐞宠揪琛ㄦ紨锛氭挱鏀鹃煶涔�0015锛岃垶韫堣€呯數鏈哄乏鍙宠浆鍔ㄧ洿鑷抽煶涔愮粨鏉熴€�"
        "褰撶敤鎴疯 '鐞宠揪' '鐞宠揪鍛�' '鐞宠揪鍦ㄥ摢閲�' '鐞宠揪鐞宠揪' 鏃惰皟鐢ㄦ宸ュ叿銆�"
        "Linda show: call when user asks about Linda.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartLindaShow();
            return std::string("{\"status\": \"linda_show_started\"}");
        });

    mcp.AddTool("cuckoo.garden_show",
        "鍥瓙琛ㄦ紨锛氭挱鏀鹃煶涔�0016锛屽皬鎻愮惔鑸垫満宸﹀彸杞姩鐩磋嚦闊充箰缁撴潫銆�"
        "褰撶敤鎴疯 '鍥瓙' '鍥瓙鍛�' '鍥瓙鍦ㄥ摢閲�' '鍥瓙鍥瓙' 鏃惰皟鐢ㄦ宸ュ叿銆�"
        "Garden show: call when user asks about garden.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartGardenShow();
            return std::string("{\"status\": \"garden_show_started\"}");
        });
}

// ============================================
// 閽熸帶 FreeRTOS 浠诲姟 (Core 1)
// ============================================
void cuckoo_clock_task(void* params) {
    // 寮€鏈鸿閲嶇疆鍘熷洜 + RTC 浜¤
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

    // Refresh audio timestamps once at startup to prevent I2S watchdog during boot
    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();

 // 寮€鏈哄悗 WiFi 宸茶繛鎺� NTPgettimeofday 鍗冲彲鑾峰彇褰撳墠鏃堕棿

 // 寮€鏈哄悗 WiFi 宸茶繛鎺� NTPgettimeofday 鍗冲彲鑾峰彇褰撳墠鏃堕棿
 // 寮€鏈哄悗 WiFi 宸茶繛鎺� NTPgettimeofday 鍗冲彲鑾峰彇褰撳墠鏃堕棿
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1000000000) {  // 2001锟斤拷锟皆猴拷说锟斤拷 NTP 锟斤拷同锟斤拷
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

 // 璁剧疆榛樿闊充箰浠ｇ悊鍦板潃鏉ヨ嚜 config.h缂栬瘧鏃跺浐瀹�
    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);

    // 浠� NVS 鍔犺浇宸蹭繚瀛樼殑闂归挓
    sm->LoadAlarmsFromNvs();

uint32_t tick_sec = 0;

 // TODO(#22): Replace 1s polling with event-driven approach:
 // - Register OnDeviceStateChanged callback for idle/active transitions
 // - Use a software timer for NTP sync instead of tick_sec counter
 // - This would let Core 1 sleep most of the time, saving power
    uint32_t sub_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        sub_tick++;

 // 妫€娴嬭澶囦粠idle鍞ら啋鐢ㄦ埛鍞ら啋AI璁板綍鏃堕棿鎴崇敤浜庨槻璇Е鍙�
        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, "Device woke up 锟斤拷 opening bird door");
                sm->OpenBirdDoor();

                // Raise wake word threshold during conversation to prevent false triggers
                Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.3f);
            } else if (sm->prev_device_state_ != (int)kDeviceStateIdle && dev_state == (int)kDeviceStateIdle) {
                ESP_LOGI(TAG, "Device sleeping 锟斤拷 closing bird door");
                sm->CloseBirdDoor();

                // Restore low threshold for easy wake-up
                Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.08f);
            }
            sm->prev_device_state_ = dev_state;

 // AI璇磋瘽鏃跺皬楦熻窡闅忚瘽闊宠妭濂忚烦鍔紙涓嶅湪琛ㄦ紨涓笖鏃犺儗鏅煶涔愭椂锛�
            if (dev_state == (int)kDeviceStateSpeaking && !sm->IsRunning() && !Application::GetInstance().GetAudioService().IsBgAudioActive()) {
                // 妫€娴嬮煶棰戣緭鍑烘椿璺冨害锛�300ms 鍐呮湁杈撳嚭 = 姝ｅ湪璇磋瘽
                int64_t ms_since_output = Application::GetInstance().GetAudioService().MsSinceLastOutput();
                if (ms_since_output < 300) {
                    // 姝ｅ湪璇磋瘽锛氱煭鑴夊啿楦熻烦锛�120ms锛�
                    sm->BirdJumpShort();
                }
                // 涓嶈璇濇椂锛坢s_since_output >= 300锛変笉璺�
            }
        }

        // === 姣忕浠诲姟锛坰ub_tick % 4 == 0锛�===
        if (sub_tick % 4 == 0) {
            tick_sec = sub_tick / 4;

        // NTP 鍚屾锛氭湭鍚屾鏃舵瘡 60 绉掗噸璇曪紝鍚屾鍚庢瘡 5 鍒嗛挓鏍″噯涓€娆�
        // 棰戠箒鏍″噯娑堥櫎杞欢鏃堕挓绱Н婕傜Щ瀵艰嚧鐨勫崐鐐�/鏁寸偣鎶ユ椂涓嶅噯
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

        // WiFi power-save now handled by CuckooBoard::SetPowerSaveLevel override
        // (intercepts LOW_POWER when music is playing)

 // Save crash log to RTC memory every 30s (RTC survives watchdog reset)
        if (tick_sec % 30 == 0) {
            auto& app = Application::GetInstance();
            rtc_crash_log.tick_sec = tick_sec;
            rtc_crash_log.dev_state = (uint8_t)app.GetDeviceState();
            rtc_crash_log.music_active = (uint8_t)app.GetAudioService().IsBgAudioActive();
            rtc_crash_log.free_heap = esp_get_free_heap_size();
        }

 // Heartbeat + stack HWM every 60s for crash diagnosis
        if (tick_sec % 60 == 0) {
            ESP_LOGI(TAG, "Heartbeat t=%ds stack_hwm=%d free_heap=%d",
                     (int)tick_sec, (int)uxTaskGetStackHighWaterMark(NULL),
                     (int)esp_get_free_heap_size());
        }

 // 闊抽鐢垫簮绠＄悊浼氬湪杈撳叆/杈撳嚭绌洪棽 15 绉掑悗鍏抽棴 I2S瀵艰嚧鍞ら啋/鍠囧彮澶辨晥
 // 闊抽鐢垫簮绠＄悊浼氬湪杈撳叆/杈撳嚭绌洪棽 15 绉掑悗鍏抽棴 I2S瀵艰嚧鍞ら啋/鍠囧彮澶辨晥
        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();
            // 璇诲彇鍏夋晱鐢甸樆鏇存柊澶滈棿妯″紡
            sm->is_dark_ = sm->CheckDark();
        }

        // 濡傛灉鏃堕棿宸查€氳繃 NTP 鎴� MCP 璁剧疆锛岀敤鍐呴儴鏃堕棿
        if (sm->time_set_) {
            // NTP 宸插悓姝ユ椂鐩存帴鐢ㄧ郴缁熸椂闂达紝閬垮厤杞欢鏃堕挓婕傜Щ瀵艰嚧鎶ユ椂涓嶅噯
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
            } else {
                // NTP 鏈悓姝ワ紙浠� MCP 璁捐繃鏃堕棿锛夛紝鍥為€€鍒拌蒋浠舵椂閽熼€掑
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

            // 鏁寸偣鍜屽崐鐐规鏌�
            int h = sm->current_hour_;
            int m = sm->current_min_;

 // 鏁寸偣鎶ユ椂姣忓垎閽� == 0 鏃惰Е鍙戠敤 NeedHourlyChime 闃查噸澶�
            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }
 // 鍗婄偣鎶ユ椂姣忓垎閽� == 30 鏃惰Е鍙戠敤 NeedHalfHourlyChime 闃查噸澶�
            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }

            // 闂归挓妫€鏌ワ紙姣忕妫€鏌ヤ竴娆★級
            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        }  // sub_tick >= 4 guard
 // Note: NTP sync handled at startup + daily tick_sec % 86400 above
    }
}
