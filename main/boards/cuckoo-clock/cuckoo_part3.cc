// ===== Part 3: LdrSensor（光敏）、BellSoundPlayer（铃声）、Dance（舞蹈） (L1925-2394) =====

/**
 * @brief 析构状态机：停止所有演出与电机，关闭电机电源
 */
CuckooStateMachine::~CuckooStateMachine() {
    StopAll();
    MotorPowerOff();
}

/**
 * @brief 打开电机电源（P-MOSFET 低电平导通 5V）
 */
void CuckooStateMachine::MotorPowerOn() {
    gpio_set_level(motor_power_pin_, 0);
}

/**
 * @brief 关闭电机电源（P-MOSFET 高电平截止）
 */
void CuckooStateMachine::MotorPowerOff() {
    gpio_set_level(motor_power_pin_, 1);
}

// ============================================
// ============================================

struct DoorOpenCtx {
    CuckooStateMachine* sm;
};

/**
 * @brief 异步开门任务：M2 正转固定时长后停止
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
    sm->door_open_ = true;
    sm->MotorPowerOff();
    ESP_LOGI(TAG, "Door open done (async)");
    vTaskDelete(NULL);
}

/**
 * @brief 播放狗叫：背景音乐激活时叠加混音，否则 OutputRawPcm 直出
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

    // 将狗叫叠加到现有背景音乐上（叠加而非替换）
    // 若背景音乐未激活（如 DogShow），则回退用 OutputRawPcm 直接播放
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
 * @brief 舞蹈出场：异步开门 + 狗尾伸出 + 小狗前进 + 狗叫 + 狗尾归零
 */
void CuckooStateMachine::RunDanceIntro() {
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open", 2048, ctx, 5, nullptr, 1);

    if (dog_servo_) {
        dog_servo_->Sweep(180, 20, (180 - 20) * 15);
    }
    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }
    PlayDogBark();
    if (dog_servo_) {
        dog_servo_->Sweep(20, 0, 20 * 15);
    }
}

/**
 * @brief 舞蹈主循环：M1 正反转交替 + 小提琴多阶段摆动 + 狗尾摇动 + 水车转动 + LED 闪烁
 * 直到音乐停止或 120 秒超时；开始前最多等 5 秒背景音乐激活。
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

    // 背景音乐可能仍在 AI 说话期间淡入：最多等 5 秒
    // 等它激活，否则下面的舞蹈循环会立即退出。
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
                    m1_stage_start_ = now;  // reset for reverse timing
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
                    m1_stage_start_ = now;  // reset for next cycle
                    m1_timer = now; m1_stage = 0;
                    m1_rand_time = 1000 + (esp_random() % 2001);
                }
                break;
        }

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
        if (led_toggle >= 6) {
            led_toggle = 0;
            led_state = !led_state;
            gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
        }

        next_frame_us += 50000;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
        }
    }
}

/**
 * @brief 舞蹈收尾：M1 净角度补偿回正、小提琴平衡、小狗后退、狗尾归位、关门
 */
void CuckooStateMachine::RunDanceFinale() {
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);

    if (m1_fwd_time_ > 0 || m1_rev_time_ > 0) {
        long net = ((long)(m1_fwd_time_ - m1_rev_time_) * 279) / 1000;
        int rem = (int)(net % 360); if (rem < 0) rem = -rem;
        int ms; bool go_fwd;
        if (rem <= 180) {
            ms = rem * 1000 / 279;
            go_fwd = (net < 0);  // net为负=反转太多，补正转
        } else {
            ms = (360 - rem) * 1000 / 279;
            go_fwd = (net >= 0);
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
        int dog_cur = dog_state_.angle;
        dog_servo_->Sweep(dog_cur, 180, (180 - dog_cur) * 15);
        dog_state_.angle = 180;
    }
    MotorPowerOn();
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
}

// ============================================
// ============================================

static const char* kAlarmNvsNamespace = "cuckoo_alarm";
static const char* kAlarmNvsKey = "alarms";

/**
 * @brief 将闹钟列表保存到 NVS
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
 * @brief 从 NVS 加载闹钟列表
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


/**
 * @brief 启动表演（整点/半点/手动触发），在 Core 1 创建 PerformanceTask
 */
void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();

    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
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
        default: is_running_ = false; return;
    }
    // 在 Core 1 上创建表演任务
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
 * @brief 表演任务线程（Core 1）
 * 整点：开门 → 鸟叫×N → 关门 → 报时铃声 → （hourly_perf 开）音乐+舞蹈 → 收尾
 * 半点：开门 → 鸟叫×3 → 关门
 * 120 秒看门狗超时强制退出，超时后恢复音频管线。
 */
void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();

    // 超时看门狗：120 秒后强制退出，防止永久卡死
    // （如音频管线死锁、互斥锁卡住、I2S 卡死）
    // 没有它，卡住的表演任务会阻塞所有报时，直到
    // 硬件看门狗约 50 分钟后才复位整个芯片。
    const TickType_t perf_start_ticks = xTaskGetTickCount();
    const TickType_t perf_timeout_ticks = pdMS_TO_TICKS(120000);
    bool perf_timed_out = false;

    auto perf_check_timeout = [&]() -> bool {
        if ((xTaskGetTickCount() - perf_start_ticks) > perf_timeout_ticks) {
            if (!perf_timed_out) {
                ESP_LOGE(TAG, "PERF TIMEOUT: aborting after 120s, type=%d",
                         (int)sm->current_performance_);
                perf_timed_out = true;
            }
            return true;
        }
        return false;
    };

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Performance: ENTER type=%d hour=%d min=%d", (int)sm->current_performance_, sm->current_hour_.load(), sm->current_min_.load());

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);
    if (perf_check_timeout()) goto perf_cleanup;

    if (sm->current_performance_ == kPerformanceHour) {
        sm->OpenBirdDoor();
        for (int i = 0; i < sm->call_count_; i++) {
            if (sm->bell_player_) {
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


        app.GetAudioService().SetOutputMuted(false);
        if (sm->hourly_perf_.load()) {
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 阶段 2：音乐 + 舞蹈（bg audio，与 start_show/LindaShow/GardenShow 同路径）
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

            unsigned long intro_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = static_cast<CuckooStateMachine*>(arg);
                s->RunDanceIntro();
                vTaskDelete(nullptr);
            }, "dance_intro", 2048, sm, 5, nullptr, 1);

            sm->RunDanceLoop();

            // 出场（开门+小狗，异步，约 5 秒）必须在收尾之前完成，
            // 否则收尾收狗/关门时会与出场动作重叠。
            {
                unsigned long since_intro = xTaskGetTickCount() * portTICK_PERIOD_MS - intro_start_ms;
                if (since_intro < 6000) vTaskDelay(pdMS_TO_TICKS(6000 - since_intro));
            }
            sm->RunDanceFinale();
        }

    // 停水车 + 关 LED
        if (sm->water_bird_) sm->water_bird_->Stop();
        gpio_set_level(LED_A_GPIO, 0);
        gpio_set_level(LED_B_GPIO, 0);

            } // end if (hourly_perf_)
} else if (sm->current_performance_ == kPerformanceHalf) {
        sm->OpenBirdDoor();
        for (int i = 0; i < sm->call_count_; i++) {
            if (sm->bell_player_) {
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

perf_cleanup:
    // ====== 完成 ======
    if (sm->mp3_) sm->mp3_->Stop();

    // 若超时，强制恢复音频管线
    if (perf_timed_out) {
        ESP_LOGW(TAG, "Perf timeout cleanup: resetting audio pipeline");
        app.GetAudioService().SetOutputMuted(false);
        app.GetAudioService().ResetDecoder();
        app.GetAudioService().ClearBackgroundAudio();
        if (sm->water_bird_) sm->water_bird_->Stop();
    }


    sm->current_phase_ = kPhaseIdle;
    sm->MotorPowerOff();
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;
    ESP_LOGI(TAG, "Perf done: kids=%d dog_done=%d dog_run=%d dance_en=%d outro_done=%d timed_out=%d", (int)sm->kids_active_.load(), (int)sm->dog_intro_done_.load(), (int)sm->dog_intro_running_.load(), sm->music_dance_enabled_, (int)sm->dog_outro_done_, (int)perf_timed_out);
    if (app.GetDeviceState() == kDeviceStateIdle) {
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
/**
 * @brief 整点/半点报时检查：实时读 LDR 判断亮度，安静模式过滤
 */
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    // 实时读取 LDR：NTP 边界补偿不能用过期的 is_dark_
    // （10 秒 tick 缓存）否则天刚变亮时可能错误跳过报时。
    int ldr_raw = ldr_ ? ldr_->ReadRaw() : -1;
    is_dark_ = (ldr_raw >= 0) ? (ldr_raw < LDR_DARK) : dark;
    ESP_LOGI(TAG, "CheckTime: %02d:%02d dark=%d LDR=%d thresh=%d quiet_mode=%d running=%d",
             hour, min, (int)is_dark_, ldr_raw, LDR_DARK, quiet_mode_.load(), (int)is_running_);
    if (is_running_) {
        ESP_LOGI(TAG, "Skipping chime: performance already running");
        return;
    }

    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle || Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        ESP_LOGI(TAG, "Skipping chime: device busy (state=%d, bg_audio=%d)", state,
                 (int)Application::GetInstance().GetAudioService().IsBgAudioActive());
        return;
    }

    int mode = quiet_mode_.load();
    if (mode == 0) {
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=0 (always silent)");
        return;
    }
    if (mode == 2 && is_dark_) {
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=2 (dark-silent) LDR=%d threshold=%d", ldr_raw, LDR_DARK);
        return;
    }
    if (mode == 2 && !is_dark_) {
        ESP_LOGI(TAG, "Chime allowed: quiet_mode=2 but LDR=%d >= threshold=%d (bright)", ldr_raw, LDR_DARK);
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
                return;
            }
        }
    }


    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting performance", hour, min);
        StartPerformance(kPerformanceHour, hour);  // bird+bell always, Phase2 skipped if hourly_perf_ disabled
        MarkHourlyChime(hour);  // dedup after successful trigger
    }
    else if (min == 30) {
        ESP_LOGI(TAG, "Half-hour chime: %d:%02d, starting mini performance", hour, min);
        StartPerformance(kPerformanceHalf, hour);
        MarkHalfHourlyChime(hour);  // dedup after successful trigger
    }
}

/**
 * @brief 设置当前时间
 * @param hour 小时
 * @param min 分钟
 * @param sec 秒
 */
void CuckooStateMachine::SetTime(int hour, int min, int sec) {
    current_hour_ = hour % 24;
    current_min_ = min % 60;
    current_sec_ = sec % 60;
    time_set_ = true;
    ESP_LOGI(TAG, "Time set to %02d:%02d:%02d", current_hour_.load(), current_min_.load(), current_sec_.load());
}

/**
 * @brief 获取当前时间
 * @param hour 输出小时
 * @param min 输出分钟
 */
void CuckooStateMachine::GetTime(int &hour, int &min) {
    hour = current_hour_;
    min = current_min_;
}

// ============================================
// ============================================
/**
 * @brief 保存安静模式设置到 NVS
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
 * @brief 从 NVS 加载安静模式设置
 */
void CuckooStateMachine::LoadQuietMode() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "qmode", &val) == ESP_OK) quiet_mode_ = (int)val;
        if (nvs_get_i8(nvs, "qstart", &val) == ESP_OK) quiet_start_ = (int)val;
        if (nvs_get_i8(nvs, "qend", &val) == ESP_OK) quiet_end_ = (int)val;
        nvs_close(nvs);
        int m = quiet_mode_.load();
        if (m == 3) {
            ESP_LOGI(TAG, "LoadQuietMode: mode=3 (time-window) start=%d end=%d (from NVS)",
                     quiet_start_.load(), quiet_end_.load());
        } else {
            ESP_LOGI(TAG, "LoadQuietMode: mode=%d (from NVS, start/end N/A)", m);
        }
    } else {
        ESP_LOGI(TAG, "LoadQuietMode: no NVS, using defaults mode=%d", quiet_mode_.load());
    }
}

/**
 * @brief 保存 kids_active_ 状态到 NVS
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
 * @brief 从 NVS 加载 kids_active_ 状态（并加载 hourly_perf）
 */
void CuckooStateMachine::LoadKidsActive() {
    LoadHourlyPerf();
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "kids", &val) == ESP_OK) kids_active_ = (bool)val;
        nvs_close(nvs);
    }
}
/**
 * @brief 保存整点报时演出开关到 NVS
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
 * @brief 从 NVS 加载整点报时演出开关
 */
void CuckooStateMachine::LoadHourlyPerf() {
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "hperf", &val) == ESP_OK) hourly_perf_ = (bool)val;
        nvs_close(nvs);
        ESP_LOGI(TAG, "LoadHourlyPerf: hourly_perf=%d (from NVS)", (int)hourly_perf_.load());
    } else {
        ESP_LOGI(TAG, "LoadHourlyPerf: no NVS, using default hourly_perf=%d", (int)hourly_perf_.load());
    }
}


// ============================================
// ============================================
/**
 * @brief 设置闹钟（重复则更新已有项）
 * @param hour 小时 (0-23)
 * @param minute 分钟 (0-59)
 * @param repeat_daily 是否每天重复
 */
void CuckooStateMachine::SetAlarm(int hour, int minute, bool repeat_daily) {
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (alarm_count_ >= kMaxAlarms) {
        ESP_LOGW(TAG, "Alarm list full (max %d)", kMaxAlarms);
        return;
    }
 // 检查重复 - 更新已有闹钟
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
 * @brief 获取闹钟列表 JSON（供 MCP 工具查询）
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
 * @param index 闹钟索引（从 1 开始）
 * @return true=删除成功，false=索引无效
 */
bool CuckooStateMachine::DeleteAlarm(int index) {
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (index < 1 || index > alarm_count_) {
        ESP_LOGW(TAG, "Invalid alarm index: %d (have %d alarms)", index, alarm_count_.load());
        return false;
    }
 // 将后面的闹钟前移
    for (int i = index - 1; i < alarm_count_ - 1; i++) {
        alarms_[i] = alarms_[i + 1];
    }
    alarm_count_--;
    ESP_LOGI(TAG, "Alarm %d deleted (remaining: %d)", index, alarm_count_.load());
    SaveAlarmsToNvs();
    return true;
}

/**
 * @brief 停止响铃中的闹钟
 */
void CuckooStateMachine::StopAlarm() {
    alarm_stopped_ = true;
    alarm_ringing_ = false;
 // 一次性闹钟：用户停止后禁用
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
 // 停止正在播放的音频
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

/**
 * @brief 检查闹钟是否到点，触发闹铃
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
 * @brief 闹钟响铃任务：响铃 50 次（每次 2 秒 ≈ 100 秒）→ 贪睡 2 分钟 → 循环，直到用户停止
 * 前 10 次响铃音量从 15% 渐增到 100%，贪睡期间每秒检查停止标志。
 */
void CuckooStateMachine::AlarmTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Alarm task started");

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    while (sm->alarm_ringing_ && !sm->alarm_stopped_) {
 // 播放闹铃 50 次（每次 2 秒 = 共响 100 秒）
 // 第一轮：前 10 次（20 秒）音量从 15% 渐增到 100%
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

 // 贪睡：等 2 分钟，每秒检查一次停止标志
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
    sm->SaveAlarmsToNvs();
}
