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
#define TAG "CuckooCtrl"

void CuckooStateMachine::DogShow() {
    if (is_running_) return;  // бУܾظ
    // ִ̨бݣ̶ Core 1ӿغģ
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);  // ɺɾ
    }, "dog_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief Сʵ
 * ̣AI˵šСɨСǰСҡͷ10С˻ءšָ
 */
void CuckooStateMachine::DogShowTask() {
    auto& app = Application::GetInstance();

    // ===  AI ˵ݺͬʱ ===
    // === ͬʱ AIֵӿȻ ===

    // === 3ʼ ===
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");

    MotorPowerOn();

    // 1. Water wheel on
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 2. Open door
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 3. Dog tail out: 180->20, 1200ms
    if (dog_servo_) {
        dog_servo_->Sweep(180, 20, 1200);
        dog_state_.angle = 20;
    }

    // 4. Dog forward
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }

    // 5. Bark #1
    PlayDogBarkDirect();

    // 6. Tail to 0 deg
    if (dog_servo_) {
        dog_servo_->Sweep(20, 0, 300);
        dog_state_.angle = 0;
    }

    // 7. Wag 10s: 0<->60
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

    // 8. Bark #2
    PlayDogBarkDirect();

    // 9. Tail back to 20 deg
    if (dog_servo_) {
        dog_servo_->Sweep(0, 20, 300);
        dog_state_.angle = 20;
    }

    // 10. Dog reverse
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }

    // 11. Tail back to 180 deg
    if (dog_servo_) {
        dog_servo_->Sweep(20, 180, 1200);
        dog_state_.angle = 180;
    }

    // 12. Stop water wheel
    if (water_bird_) water_bird_->Stop();

    // 13. Close door
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // Cleanup
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
 * @brief ͨWAVļ
 * @param filename WAVļ dog_bark.wav, 2001.wav~2005.wav
 * WAVͷȡʺݿ飬ͨOutputRawPcmֱӲ
 * ǹļŴƫС⣩
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

    // Find data chunk (may have fmt, LIST etc before it)
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
        // DogShow path: no bg audio, use OutputRawPcm directly
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

// ֱӲŹУ PlayWavAsset
void CuckooStateMachine::PlayDogBarkDirect() {
    PlayWavAsset("dog_bark.wav");
}

bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;

    //  MP3  PSRAM
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

        // ز 2205016000
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

        //  drain ݣֹ ring buffer 
        while (audio.GetBgAudioFillLevel() > 64000 && is_running_)
            vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(pcm);
    // Push done: wait for ring buffer to drain, then clear bg audio flags
    // so show loops watching IsBgAudioActive() exit when music really ends.
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
 * @brief մݣMCPڣ
 */
void CuckooStateMachine::StartLindaShow() {
    // MCP ڣءʵʱںִ̨С
    if (is_running_) return;  // бУܾ
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->LindaShow();
        vTaskDelete(nullptr);
    }, "linda_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief ԰ӳݣMCPڣ
 */
void CuckooStateMachine::StartGardenShow() {
    // MCP ڣءʵʱںִ̨С
    if (is_running_) return;  // бУܾ
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->GardenShow();
        vTaskDelete(nullptr);
    }, "garden_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief մʵ
 * ̣LED0015.mp3š赸+LED˸ǰ24룩LEDȫֽлLEDָ
 */
void CuckooStateMachine::LindaShow() {
    if (is_running_) { ESP_LOGW(TAG, "LindaShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

    //  AI ˵ݺͬʱУֵӿ

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");

    // 1. LEDһʾݼʼ
    gpio_set_level(LED_A_GPIO, 1);  // LED_A 
    gpio_set_level(LED_B_GPIO, 1);  // LED_B 
    vTaskDelay(pdMS_TO_TICKS(500));  // 0.5

    // 2. bg audioTTSI2S
    if (mp3_) { mp3_->SetDisableDucking(true); int song = LINDA_SONG_BASE + (linda_song_counter_++ % LINDA_SONG_COUNT); ESP_LOGI(TAG, "LindaShow: playing %04d.mp3 (counter=%d)", song, (int)linda_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }

    // Wait for bg audio to start draining
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED˸ + 赸תֽ
    //    ǰ24룺LED˸ + 赸
    //    24LEDȫ赸ֱֽ
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long m1_timer = music_start_ms;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;
    m1_fwd_time_ = 0;  // תۼʱ
    m1_rev_time_ = 0;  // תۼʱ
    m1_stage_start_ = music_start_ms;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s safety timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        // ֹͣǰԼ2루24ʱLEDȫֹͣ˸
        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

        // 赸1~3룬Ȼ
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
        // LED˸ǰ24룬ÿ6֡лһ = Լ300ms
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

    // 5. ֽ  赸ͣ  Ÿл  LEDϨ
    if (m1_) m1_->Stop();  // ͣ赸

    // ڽǶȲ279/s
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
            m1_->Stop();
        }
    }

    // лֻţ0~4ѭ
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "LindaShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

    gpio_set_level(LED_A_GPIO, 0);  // LED_A 
    gpio_set_level(LED_B_GPIO, 0);  // LED_B 

    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // bg audio
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);  // ָ duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "LindaShow: done");
}

/**
 * @brief ԰ӱʵ
 * ̣LED0016.mp3šСٶ0~180Ȱڶ+LED˸ǰ24룩LEDȫֽλлLEDָ
 */
void CuckooStateMachine::GardenShow() {
    if (is_running_) { ESP_LOGW(TAG, "GardenShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

    //  AI ˵ݺͬʱУֵӿ

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");

    // 1. LEDһʾݼʼ
    gpio_set_level(LED_A_GPIO, 1);  // LED_A 
    gpio_set_level(LED_B_GPIO, 1);  // LED_B 
    vTaskDelay(pdMS_TO_TICKS(500));  // 0.5

    // 2. bg audioTTSI2S
    if (mp3_) { mp3_->SetDisableDucking(true); int song = GARDEN_SONG_BASE + (garden_song_counter_++ % GARDEN_SONG_COUNT); ESP_LOGI(TAG, "GardenShow: playing %04d.mp3 (counter=%d)", song, (int)garden_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }

    // Wait for bg audio to start draining
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED˸ + Сٶֱֽ
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

        // : ÿ50ms֡ƶ9, 0~140Ұڶ
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

    // 5. ֽ  Сƽ  Ÿл  LEDϨ
    if (violin_servo_) {
        violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000);  // ӵǰǶƽص90
    }

    // лֻţ0~4ѭ
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "GardenShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

    gpio_set_level(LED_A_GPIO, 0);  // LED_A 
    gpio_set_level(LED_B_GPIO, 0);  // LED_B 

    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // bg audio
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);  // ָ duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "GardenShow: done");
}

void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Dance start (Arduino-style)");

    // M1赸: ת1-3  ͣ20ms  ת1-3  ͣ20ms  ѭ8
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

 // : (board B M2 via violin_motor_)
        if (violin_motor_) violin_motor_->Forward(VIOLIN_WARMUP_PERCENT);

        // Сٶ: 90018090 ѭڶ
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

// ============================================
// ٶȲԣ赸תָ
// : cuckoo.motor_test (seconds: 1~60)
// ============================================
void CuckooStateMachine::MotorTest(int seconds) {
    if (is_running_) return;
    is_running_ = true;
    MotorPowerOn();
    ESP_LOGI(TAG, "MotorTest: M1 forward %d seconds...", seconds);
    if (m1_) m1_->Forward(100);  // GPIOֱȫѹ
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    if (m1_) m1_->Stop();
    MotorPowerOff();
    is_running_ = false;
    ESP_LOGI(TAG, "MotorTest: done");
}


/**
 * @brief ۺϱڣMCP cuckoo.start_show
 * Core 1cuckoo_show  StartShowTask
 */
void CuckooStateMachine::StartShow() {
    // /㱨ʱ  Show AI 
    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }
    // 壩ͣٿʼ
    if (is_running_) {
        StopAll();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ռãֹظ
    is_running_ = true;
    current_performance_ = kPerformanceManual;

    // 첽ִУMCP ߵѭ߳ϣapp.Schedule
    // ѭȴ AbortSpeaking/TTS-STOP
    // Schedule صŲ϶ӣTTS-STOP ״̬ת޷ִУ
    // ԰ŵ Core 1 ̨
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
 * @brief ۺϱݺ̨
 * AILED+ˮѡ֡+С(첽)赸ѭLEDšָ
 */
void CuckooStateMachine::StartShowTask() {
    auto& app0 = Application::GetInstance();
    // Start immediately — ShowTask now uses PlayShowMusicBg (bg audio ring buffer)
    // which the audio service mixes with TTS via ducking, no I2S conflict.
    ESP_LOGI(TAG, "Show: starting immediately (bg audio + ducking handles overlap)");

    app0.GetAudioService().SetWakeWordThreshold(0.30f);

MotorPowerOn();
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

    ShowTask(this);
}

/**
 * @brief ۺϱִУShowTaskڵõʵ߼
 * PerformanceTaskRunDanceIntro/RunDanceLoop/RunDanceFinale
 */
void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Show task started");

    auto& app = Application::GetInstance();

    //  AIݺͬʱ
    if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // ֲ

    // ====== Show start: LEDs on + water wheel ======
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // ˮ IN1=HIGH

    // ѡ?
    int next = (esp_random() % 12) + 1;
    if (next == sm->show_music_index_ && next < 12) {
        next++;
    } else if (next == sm->show_music_index_) {
        next = 1;
    }
    sm->show_music_index_ = next;
    // Use PlayShowMusicBg (bg audio ring buffer) like LindaShow/GardenShow
    // so audio service mixes music with TTS via ducking — no I2S conflict.
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

    // 첽+С赸ѭ
    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);

    // 赸+С̿ʼͿ/СУ
    sm->RunDanceLoop();

    // ָֽѴʺ
    // ע⣺StartShow  AbortSpeaking ״̬ listening idle
    // ֱӻָᵼ»Ѵ listening ״̬ AbortSpeaking  audio_input 


    if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // ָ duck

    // ȷ豸ص idle ٻָ
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "Show: music ended, running dance finale");

    sm->RunDanceFinale();

    // Clean up bg audio AFTER finale completes (previously it was before, causing state gap)
    app.GetAudioService().EnableBgAudioDrain(false);
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "Show: bg audio cleaned up, wake word threshold will be restored by clock task");
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;

    // ====== Stop water wheel + LEDs off ======
    if (sm->water_bird_) sm->water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    sm->MotorPowerOff();

    // ָѴֵ豸idleӿزת
    // Fix(2026-07-19): show runs while device stays idle, no state transition,
    // clock task never restores 0.02 -> was stuck at 0.30 (hard to wake).
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    else
        app.GetAudioService().SetWakeWordThreshold(0.30f);

    ESP_LOGI(TAG, "Show task finished");
    vTaskDelete(NULL);
}

void CuckooStateMachine::PlayMusic(int index) {
    if (mp3_) mp3_->PlayBgMusic(index);
}

/**
 * @brief ֹͣеͱ
 * - ͣеM1~M4 + С + ˮ
 * - 90
 * - LEDϨ
 * - ֲͣǲǱģʽ
 * - ͨSchedule첽豸״̬ΪIdle
 */
void CuckooStateMachine::StopAll() {
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
    if (m1_) m1_->Stop();  // 赸
    if (m2_) m2_->Stop();  // 
    if (m3_) m3_->Stop();  // С
    if (m4_) m4_->Stop();  // 
    if (violin_motor_) violin_motor_->Stop();  // С
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    //  stop_all+play_url طŴ
    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
            mp3_->Stop();
        }
    }

    // ֱ AbortSpeaking TTS Ȼ
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
 * @brief ֲֹͣţƵ
 */
void CuckooStateMachine::StopMusic() {
    kids_dance_ = false;
    if (mp3_) {
        mp3_->Stop();
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

/**
 * @brief Ŵ򿪣M2תʱ MAIN_DOOR_TIME_MS
 */
void CuckooStateMachine::OpenDoor() {
    MotorPowerOn();
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
    door_open_ = true;
    ESP_LOGI(TAG, "Door: OPEN (door_open_=%d)", (int)door_open_.load());
    MotorPowerOff();
}

/**
 * @brief ŹرգM2תʱ MAIN_DOOR_TIME_MS
 */
void CuckooStateMachine::CloseDoor() {
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
    door_open_ = false;
    kids_dance_ = false;
    ESP_LOGI(TAG, "Door: CLOSED (door_open_=%d)", (int)door_open_.load());
    MotorPowerOff();
}

/**
 * @brief СһԾͨ500ms + ϵȴ400ms
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
 * @brief СԾAI˵ʱ滰ࣩ
 * 50-200msȣȴ300-700ms
 */
void CuckooStateMachine::BirdJumpShort() {
    // AI˵ʱС滰Ծģʽ
    // 50-200msģȻԾ
    // ȴ300-700ms̫
    MotorPowerOn();
    if (water_bird_) water_bird_->Stop();  // ͣˮͷGPIO39ͻ
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // ͨ磬С񵯳
    int pulse = 50 + (esp_random() % 151);  // 50-200ms
    vTaskDelay(pdMS_TO_TICKS(pulse));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // ϵ磬С
    int cooldown = 300 + (esp_random() % 401);  // 300-700msȴʱ
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

