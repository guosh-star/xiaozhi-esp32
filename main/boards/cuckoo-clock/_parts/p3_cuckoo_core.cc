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

CuckooStateMachine::~CuckooStateMachine() {
    StopAll();
    MotorPowerOff();
}

/**
 * @brief 򿪵ԴP-MOSFET͡5Vͨ
 */
void CuckooStateMachine::MotorPowerOn() {
    gpio_set_level(motor_power_pin_, 0);
}

void CuckooStateMachine::MotorPowerOff() {
    gpio_set_level(motor_power_pin_, 1);
}

// ============================================
// 赸У㱨ʱ PerformanceTask + ۺϱ ShowTask ã
// ============================================

// 赸У㱨ʱ PerformanceTask + ۺϱ ShowTask ã
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
    sm->door_open_ = true;
    sm->MotorPowerOff();
    ESP_LOGI(TAG, "Door open done (async)");
    vTaskDelete(NULL);
}

void CuckooStateMachine::PlayDogBark() {
    //  assets ȡ dog_bark.wav (16kHz mono s16)
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

    // ѹ PCM ص Mp3PlayerDecodeSingleFile ѭԶ
    const int16_t* pcm = (const int16_t*)(wav_data + 44);
    size_t pcm_bytes = wav_size - 44;
    size_t num_samples = pcm_bytes / sizeof(int16_t);

    // Mix dog bark on top of existing bg audio (overlap, not replace)
    // or fall back to OutputRawPcm if bg audio is not active (e.g. DogShow)
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

void CuckooStateMachine::RunDanceIntro() {
    // ѹ PCM ص Mp3PlayerDecodeSingleFile ѭԶ
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open", 2048, ctx, 5, nullptr, 1);

    // 첽ţ赸/ֲУ
    if (dog_servo_) {
        dog_servo_->Sweep(180, 20, (180 - 20) * 15);
    }
    // Сǰ  ͣ  Уͱͬʱ
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }
    PlayDogBark();
    // Сǰ  ͣ  Уͱͬʱ
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
    m1_fwd_time_ = 0;
    m1_rev_time_ = 0;
    m1_stage_start_ = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Bg music may still be fading in while AI speaks: wait up to 5s for it
    // to become active, otherwise the dance loop below exits instantly.
    for (int w = 0; w < 100 && is_running_
        && !Application::GetInstance().GetAudioService().IsBgAudioActive()
        && (!mp3_ || !mp3_->IsPlaying()); w++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    unsigned long dance_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (is_running_ && (Application::GetInstance().GetAudioService().IsBgAudioActive()
           || (mp3_ && mp3_->IsPlaying()))  // also covers old PlayBgMusic path
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - dance_start_ms) < 120000UL) {  // 120s safety timeout
        if (water_bird_ && !kids_dance_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // M1 赸: ת/ת棨1~3룬>3
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
                    m1_rand_time = 1000 + (esp_random() % 2001);  // 1~3
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
                    m1_rand_time = 1000 + (esp_random() % 2001);  // 1~3
                }
                break;
        }

        // Сѭÿ֡ 9 ƽڶ
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

        // Сҡβ
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

        // LED ˸ÿ6֡ ~300ms
        led_toggle++;
        if (led_toggle >= 6) {
            led_toggle = 0;
            led_state = !led_state;
            gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
        }

        // 50ms ֡ʿ
        next_frame_us += 50000;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
        }
    }
}

void CuckooStateMachine::RunDanceFinale() {
    // ֽһֱŹرպ ShowTask ĩβϨ
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);

    // ڽǶȲ279/s40s31Ȧʵ⣩ֻ360ڵĲ
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

    // ƽСת
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

    // С˻
    if (m3_) {
        m3_->Reverse(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(920));
        m3_->Stop();
    }
    // СӵǰǶȹλ180㣨䣩
    if (dog_servo_) {
        int dog_cur = dog_state_.angle;
        dog_servo_->Sweep(dog_cur, 180, (180 - dog_cur) * 15);
        dog_state_.angle = 180;
    }
    // 
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
}

// ============================================
// NVS ӳ־û
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
    // ͳЧʱ
    int count = 0;
    for (int i = 0; i < kMaxAlarms; i++) {
        if (alarms_[i].enabled) count++;
    }
    alarm_count_ = count;
    ESP_LOGI(TAG, "Alarms loaded from NVS (%d active)", count);
}


/**
 * @brief ʱݣ//ֶ
 * @param type ͣkPerformanceHour()/kPerformanceHalf()/kPerformanceManual(ֶ)
 * @param hour Сʱ㱨ʱã
 * - AI󴥷5ںԵ
 * - ȴAIԻAbortSpeakingֹͻѴʼ
 * - Core 1PerformanceTaskִ
 */
void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();

    // AI󴥷豸մidle5 performanceAIڻʱԶcuckoo.performance
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {  // 5봰
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }

    //  AI ˵/ȴ
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
        vTaskDelay(pdMS_TO_TICKS(200));  // ״̬ص idle
    }
    // ڼֹΪѴʻ AI
    app.GetAudioService().EnableVoiceProcessing(false);
    // ʽѴʼ⣨ɰ汾 EnableVoiceProcessing ܲ߼
    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
            total_calls_ = hour;       // ¼ܱʱ㣩
            call_count_ = 3;           // 㱨ʱ3ν
            break;
        case kPerformanceHalf:
            total_calls_ = 0;
            call_count_ = 3;           // 㱨ʱ3ν
            break;
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
        default: is_running_ = false; return;
    }
    // Create PerformanceTask on Core 1
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
 * @brief ʱšС+СŹء+赸
 * - 㣺N+0013.mp3ѭN+赸LED+ˮ+赸+С+С
 * - 㣺3Σֺ赸
 * - ָѴʺ
 */
void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Performance: ENTER type=%d hour=%d min=%d", (int)sm->current_performance_, sm->current_hour_.load(), sm->current_min_.load());

    //  AI ղŶзֹ Opus 
    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    // ====== 㱨ʱ򻯰棨ްװֻƵ̣======
    if (sm->current_performance_ == kPerformanceHour) {
        // Phase 1: Ŵ  С+  Źر
        sm->OpenBirdDoor();  //  (m4_)
        for (int i = 0; i < sm->call_count_; i++) {
            // 첽ƽ+Ͷͬ
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundAsync();
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // ý
            sm->BirdJumpPulse();  // 壨ͬʱڲ
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(1800));  // Ƚ(~1.76s)
            }
        }
        // һٹ
        vTaskDelay(pdMS_TO_TICKS(2000));
        sm->CloseBirdDoor();  // Źر

        // һԽ뵽PSRAMOutputRawPcmظţMP3򿪵Ķ
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            int16_t* bell_pcm = nullptr;
            size_t bell_samples = 0;
            int bell_sr = 0;
            int dec_ret = sm->mp3_->DecodeToBuffer(13, &bell_pcm, &bell_samples, &bell_sr);
            if (dec_ret == 0 && bell_pcm && bell_samples > 0) {
                ESP_LOGI(TAG, "Bell PCM loaded: %u samples @ %d Hz", (unsigned)bell_samples, bell_sr);
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    app.GetAudioService().OutputRawPcm(bell_pcm, bell_samples, bell_sr);
                    // OutputRawPcm ģŷ
                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                heap_caps_free(bell_pcm);
            } else {
    //  PlayIndex
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


 // Phase 3: ָAIƵ
        app.GetAudioService().SetOutputMuted(false);  // ָAIƵ
        if (sm->hourly_perf_.load()) {
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // ˮ IN1=HIGH

    // Phase 2: music + dance (bg audio, same path as start_show/LindaShow/GardenShow)
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

            // 첽+С赸ѭ ShowTask һ£
            unsigned long intro_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = static_cast<CuckooStateMachine*>(arg);
                s->RunDanceIntro();
                vTaskDelete(nullptr);
            }, "dance_intro", 2048, sm, 5, nullptr, 1);

            // 赸+С̿ʼͿ/СУ
            sm->RunDanceLoop();

            // Intro (door+dog, async, ~5s) must finish before the finale
            // retracts the dog / closes the door, or the sequences overlap.
            {
                unsigned long since_intro = xTaskGetTickCount() * portTICK_PERIOD_MS - intro_start_ms;
                if (since_intro < 6000) vTaskDelay(pdMS_TO_TICKS(6000 - since_intro));
            }
            sm->RunDanceFinale();
        }

    // Stop water wheel + LEDs off
        if (sm->water_bird_) sm->water_bird_->Stop();
        gpio_set_level(LED_A_GPIO, 0);
        gpio_set_level(LED_B_GPIO, 0);

    // ====== 㱨ʱ+С+һ ======
            } // end if (hourly_perf_)
} else if (sm->current_performance_ == kPerformanceHalf) {
        sm->OpenBirdDoor();  // Ŵ
        for (int i = 0; i < sm->call_count_; i++) {
            // 첽ƽ+Ͷͬ
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundAsync();
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // ý
            sm->BirdJumpPulse();  // 壨ͬʱڲ
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(1800));  // Ƚ(~1.76s)
            }
        }
        // һٹ
        vTaskDelay(pdMS_TO_TICKS(2000));
        sm->CloseBirdDoor();  // Źر
    // ָ AI
        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== Done ======
    if (sm->mp3_) sm->mp3_->Stop();


    sm->current_phase_ = kPhaseIdle;
    sm->MotorPowerOff();
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    ESP_LOGI(TAG, "Perf done: kids=%d dog_done=%d dog_run=%d dance_en=%d outro_done=%d", (int)sm->kids_active_.load(), (int)sm->dog_intro_done_.load(), (int)sm->dog_intro_running_.load(), sm->music_dance_enabled_, (int)sm->dog_outro_done_);
    // ָѴʼ⣨ EnableVoiceProcessing   ResetDecoder  TTS
    // ֻ idle ״̬Żָ listening ״̬󴥷Ѵ
    if (app.GetDeviceState() == kDeviceStateIdle) {
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
/**
 * @brief 鲢/㱨ʱ
 * - ҹ(22:00-6:00)ʱ
 * - AIæʱ򱳾Ƶвʱ
 * -  (min==0):  StartPerformance(kPerformanceHour)
 * -  (min==30):  StartPerformance(kPerformanceHalf)
 */
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    is_dark_ = dark;
    if (is_running_) {
        ESP_LOGI(TAG, "Skipping chime: performance already running");
        return;
    }

    // AI æʱ򱳾Ƶвʱ
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle || Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        ESP_LOGI(TAG, "Skipping chime: device busy (state=%d, bg_audio=%d)", state,
                 (int)Application::GetInstance().GetAudioService().IsBgAudioActive());
        return;
    }

    // ģʽж
    int mode = quiet_mode_.load();
    if (mode == 0) {
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=0 (always silent)");
        return;  // ȫ쾲
    }
    if (mode == 2 && is_dark_) {
        int ldr_raw = ldr_ ? ldr_->ReadRaw() : -1;
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=2 (dark-silent) LDR=%d threshold=%d", ldr_raw, LDR_DARK);
        return;  // ھ
    }
    if (mode == 3) {
        int start = quiet_start_.load();
        int end = quiet_end_.load();
        if (start < end) {
            if (hour >= start && hour < end) {
                ESP_LOGI(TAG, "Skipping chime: quiet_mode=3 window %d-%d", start, end);
                return;
            }
        } else {
            if (hour >= start || hour < end) {
                ESP_LOGI(TAG, "Skipping chime: quiet_mode=3 overnight window %d-%d", start, end);
                return;  // ҹ: 22~6
            }
        }
    }
    // mode==1 ȫ챨ʱreturn

    // 㱨ʱ

    // 㱨ʱ
    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting performance", hour, min);
        StartPerformance(kPerformanceHour, hour);  // bird+bell always, Phase2 skipped if hourly_perf_ disabled
        MarkHourlyChime(hour);  // dedup after successful trigger
    }
    // 㱨ʱ
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
// ģʽ NVS 洢
// ============================================
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

void CuckooStateMachine::SaveKidsActive() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i8(nvs, "kids", (int8_t)kids_active_.load());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void CuckooStateMachine::LoadKidsActive() {
    LoadHourlyPerf();
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "kids", &val) == ESP_OK) kids_active_ = (bool)val;
        nvs_close(nvs);
    }
}
void CuckooStateMachine::SaveHourlyPerf() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i8(nvs, "hperf", (int8_t)hourly_perf_.load());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void CuckooStateMachine::LoadHourlyPerf() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "hperf", &val) == ESP_OK) hourly_perf_ = (bool)val;
        nvs_close(nvs);
    }
}


// ============================================
// 幦  ֧5ӣNVS־û
// ============================================
/**
 * @brief 
 * @param hour Сʱ (0-23)
 * @param minute  (0-59)
 * @param repeat_daily true=ÿظ, false=һ
 * Զȥأͬʱӻrepeat_daily
 */
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

/**
 * @brief ȡбJSON
 * @return JSONַ[{"index":1,"hour":8,"minute":0,"enabled":true,"repeat_daily":true},...]
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

/**
 * @brief ֹͣ
 * һֹͣԶãظӱ
 */
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
    SaveAlarmsToNvs();  // persist disabled one-shot alarm
 // Stop any playing audio
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

/**
 * @brief ÿӴ
 * - ʱƥ==0ʱ
 * - AlarmTaskں̨
 * - вظ
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
 * @brief 岥񣺷50֣100룩15%ǿ100%ÿ֮ͣ2
 * ڼalarm_stopped_־ûֹͣ˳
 */
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
                    volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% ǿ 100% (ǰ10)
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
    sm->SaveAlarmsToNvs();
}

// ============================================
// ɫݣ MCP ߵʵ֣
// - DogShow: СݣšܳСҡͷ˻ءţ
// - LindaShow: մ赸ݣ0015 + 赸 + LED˸
// - GardenShow: ԰Lindaݣ0016 + Сٶ + LED˸
// ============================================

// DogShow()  MCP ڣز
// ʵʱ DogShowTask() ִУ Core 1 ̨
/**
 * @brief СݣMCPڣ
 * Core 1dog_showʵDogShowTask()ִ
 */
