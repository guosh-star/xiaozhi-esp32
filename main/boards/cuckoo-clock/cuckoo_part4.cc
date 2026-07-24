// ===== Part 4: 演出/报时/闹钟/狗秀/琳达/花园 (L2395-4412) =====
/**
 * @brief 启动表演（整点/半点/手动触发）
 *
 * @param type 表演类型：kPerformanceHour(整点)/kPerformanceHalf(半点)/kPerformanceManual(手动)
 * @param hour 小时（半点和手动报时不需此参数）
 *
 * - AI 触发后 5 秒内忽略重复触发
 * - 先用 AbortSpeaking 终止 TTS，防止冲突
 * - 在 Core 1 上创建 PerformanceTask 执行
 */
void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();

    // ��AI�󴥷��豸�մ�idle����5�������� performanceAI�����ڻ���ʱ�Զ���cuckoo.performance
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {  // 5�봰��
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }

    // ��� AI ����˵��/�����ȴ����
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
        vTaskDelay(pdMS_TO_TICKS(200));  // ��״̬�ص� idle
    }
    // �����ڼ��������������ֹ����������Ϊ���Ѵʻ��� AI
    app.GetAudioService().EnableVoiceProcessing(false);
    // ��ʽ�������Ѵʼ�⣨�ɰ汾 EnableVoiceProcessing ���ܲ��������߼���
    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
            total_calls_ = hour;       // ��¼�ܱ�ʱ��������㣩
            call_count_ = 3;           // ���㱨ʱ��3�ν���
            break;
        case kPerformanceHalf:
            total_calls_ = 0;
            call_count_ = 3;           // ��㱨ʱ��3�ν���
            break;
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
        default: is_running_ = false; return;
    }
 // ��Core 1����PerformanceTask
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
 * @brief ��ʱ�����������š�С����+�С����Źء���������+�赸������
 * - ���㣺���N��+0013.mp3ѭ��N��+�赸��LED+ˮ��+�赸���+С����+С����
 * - ��㣺���3�Σ������ֺ��赸
 * - ������ָ����Ѵʺ���������
 */
void CuckooStateMachine::PerformanceTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Performance task started");

    // ���� AI �����ղ��Ŷ��з�ֹ������ Opus ����
    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    // ====== ���㱨ʱ�򻯰棨����ް�װ��ֻ����Ƶ���̣�======
    if (sm->current_performance_ == kPerformanceHour) {
        // Phase 1: ���Ŵ� �� С������+�� �� ���Źر�
        sm->OpenBirdDoor();  // ���� (m4_)
        for (int i = 0; i < sm->call_count_; i++) {
            // �첽�ƽ���+��������������������Ͷ�������ͬ��
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundAsync();
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // �ý�������������
            sm->BirdJumpPulse();  // ��������壨����ͬʱ�ڲ���
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(1800));  // �Ƚ�������(~1.76s)
            }
        }
        // �����һ�������ٹ���
        vTaskDelay(pdMS_TO_TICKS(2000));
        sm->CloseBirdDoor();  // ���Źر�

        // һ���Խ��뵽PSRAM����OutputRawPcm�ظ����ţ�����MP3�����������򿪵Ķ���
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            int16_t* bell_pcm = nullptr;
            size_t bell_samples = 0;
            int bell_sr = 0;
            int dec_ret = sm->mp3_->DecodeToBuffer(13, &bell_pcm, &bell_samples, &bell_sr);
            if (dec_ret == 0 && bell_pcm && bell_samples > 0) {
                ESP_LOGI(TAG, "Bell PCM loaded: %u samples @ %d Hz", (unsigned)bell_samples, bell_sr);
                for (int i = 0; i < sm->total_calls_ && sm->is_running_; i++) {
                    app.GetAudioService().OutputRawPcm(bell_pcm, bell_samples, bell_sr);
                    // OutputRawPcm �������ģ�����ŷ���
                    if (!sm->is_running_) break;
                    if (i < sm->total_calls_ - 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                heap_caps_free(bell_pcm);
            } else {
    // ������ PlayIndex
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


 // Phase 3: �ָ�AI��Ƶ���
        app.GetAudioService().SetOutputMuted(false);  // �ָ�AI��Ƶ
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // ˮ�� IN1=HIGH

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

            // �첽��������+С���������������赸ѭ������ ShowTask һ�£�
            unsigned long intro_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = static_cast<CuckooStateMachine*>(arg);
                s->RunDanceIntro();
                vTaskDelete(nullptr);
            }, "dance_intro", 2048, sm, 5, nullptr, 1);

            // �赸+С�������̿�ʼ���Ϳ���/С���������У�
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

    // ====== ��㱨ʱ������+С������+��һ�� ======
    } else if (sm->current_performance_ == kPerformanceHalf) {
        sm->OpenBirdDoor();  // ���Ŵ�
        for (int i = 0; i < sm->call_count_; i++) {
            // �첽�ƽ���+��������������������Ͷ�������ͬ��
            if (sm->bell_player_) {
                sm->bell_player_->PlayCuckooSoundAsync();
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // �ý�������������
            sm->BirdJumpPulse();  // ��������壨����ͬʱ�ڲ���
            if (i < sm->call_count_ - 1) {
                vTaskDelay(pdMS_TO_TICKS(1800));  // �Ƚ�������(~1.76s)
            }
        }
        // �����һ�������ٹ���
        vTaskDelay(pdMS_TO_TICKS(2000));
        sm->CloseBirdDoor();  // ���Źر�
    // �����ָ� AI
        app.GetAudioService().SetOutputMuted(false);
    }

    // ====== Done ======
    if (sm->mp3_) sm->mp3_->Stop();


    sm->current_phase_ = kPhaseIdle;
    sm->MotorPowerOff();
    // �ָ����Ѵʼ�⣨���� EnableVoiceProcessing �� ���� ResetDecoder ���� TTS��
    // ֻ�� idle ״̬�Żָ��������� listening ״̬�󴥷����Ѵ�
    if (app.GetDeviceState() == kDeviceStateIdle) {
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
/**
 * @brief ��鲢��������/��㱨ʱ
 * - ҹ��(22:00-6:00)����ʱ
 * - AIæʱ�򱳾���Ƶ�����в���ʱ
 * - ���� (min==0): ���� StartPerformance(kPerformanceHour)
 * - ��� (min==30): ���� StartPerformance(kPerformanceHalf)
 */
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    is_dark_ = dark;
    if (is_running_) {
        ESP_LOGI(TAG, "Skipping chime: performance already running");
        return;
    }

    // AI æʱ�򱳾���Ƶ�����в���ʱ
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle || Application::GetInstance().GetAudioService().IsBgAudioActive()) {
        ESP_LOGI(TAG, "Skipping chime: device busy (state=%d, bg_audio=%d)", state,
                 (int)Application::GetInstance().GetAudioService().IsBgAudioActive());
        return;
    }

    // ����ģʽ�ж�
    int mode = quiet_mode_.load();
    if (mode == 0) {
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=0 (always silent)");
        return;  // ȫ�쾲��
    }
    if (mode == 2 && is_dark_) {
        int ldr_raw = ldr_ ? ldr_->ReadRaw() : -1;
        ESP_LOGI(TAG, "Skipping chime: quiet_mode=2 (dark-silent) LDR=%d threshold=%d", ldr_raw, LDR_DARK);
        return;  // ��ھ���
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
                return;  // ��ҹ: 22~6
            }
        }
    }
    // mode==1 ȫ�챨ʱ����return

    // ���㱨ʱ

    // ���㱨ʱ
    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting performance", hour, min);
        if (hourly_perf_.load()) { StartPerformance(kPerformanceHour, hour); } else { ESP_LOGI(TAG, "Hourly chime: performance disabled"); }
        MarkHourlyChime(hour);  // dedup after successful trigger
    }
    // ��㱨ʱ
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
// ����ģʽ NVS �洢
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
    nvs_handle_t nvs;
    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {
        int8_t val;
        if (nvs_get_i8(nvs, "kids", &val) == ESP_OK) kids_active_ = (bool)val;
        nvs_close(nvs);
    }
}

// ============================================
// ���幦�� �� ֧�����5�����ӣ�NVS�־û�
// ============================================
/**
 * @brief ��������
 * @param hour Сʱ (0-23)
 * @param minute ���� (0-59)
 * @param repeat_daily true=ÿ���ظ�, false=һ����
 * �Զ�ȥ�أ�ͬʱ������ӻ����repeat_daily
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
 * @brief ��ȡ�����б�JSON
 * @return JSON�����ַ�����[{"index":1,"hour":8,"minute":0,"enabled":true,"repeat_daily":true},...]
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
 * @brief ֹͣ�������������
 * һ��������ֹͣ���Զ����ã��ظ����ӱ�������
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
 // Stop any playing audio
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

/**
 * @brief ÿ�������Ӵ���
 * - ʱ��ƥ������==0ʱ����
 * - ����AlarmTask�ں�̨��������
 * - �����в����ظ�����
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
 * @brief ���岥�����񣺷���50�֣�100�룩��������15%��ǿ��100%��ÿ��֮����ͣ2����
 * �ڼ���alarm_stopped_��־�����û�ֹͣ�������˳�
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
                    volume = 0.15f + 0.85f * (float)i / 9.0f;  // 15% ��ǿ�� 100% (ǰ10��)
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
// ��ɫ���ݣ����� MCP ���ߵ�ʵ�֣�
// - DogShow: С���������ݣ����š��ܳ����С�ҡͷ���˻ء����ţ�
// - LindaShow: �մ��赸���ݣ�����0015 + �赸��� + LED��˸��
// - GardenShow: ԰��Linda�������ݣ�����0016 + С���ٶ�� + LED��˸��
// ============================================

// DogShow() �� MCP ������ڣ��������ز�������
// ʵ�ʱ����� DogShowTask() ��ִ�У����� Core 1 ��̨�����
/**
 * @brief С���������ݣ�MCP��ڣ���������
 * ��Core 1����dog_show����ʵ����DogShowTask()ִ��
 */
void CuckooStateMachine::DogShow() {
    if (is_running_) return;  // ���б��������У��ܾ��ظ�����
    // ������̨����ִ�б��ݣ��̶��� Core 1���ӿغ��ģ�
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);  // ������ɺ���ɾ������
    }, "dog_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief С����������ʵ��
 * ���̣���AI˵��������������������š�С�����ɨ����С��ǰ�����С�ҡͷ10����С��˻ء����š��ָ�
 */
void CuckooStateMachine::DogShowTask() {
    auto& app = Application::GetInstance();

    // === ����� AI ˵�������ݺ�����ͬʱ���� ===
    // === ����ͬʱ����� AI����ֵ���ӿ�������Ȼ���� ===

    // === ��3������ʼ���� ===
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
 * @brief ͨ��WAV�ļ�������
 * @param filename WAV�ļ������� dog_bark.wav, 2001.wav~2005.wav��
 * ����WAVͷ��ȡ�����ʺ����ݿ飬ͨ��OutputRawPcmֱ�Ӳ���
 * �ǹ����ļ�������Ŵ���������ƫС���⣩
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

// ֱ�Ӳ��Ź��У���������������� PlayWavAsset
void CuckooStateMachine::PlayDogBarkDirect() {
    PlayWavAsset("dog_bark.wav");
}

bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;

    // ���� MP3 �� PSRAM
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

        // �������ز��� 22050��16000
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

        // �� drain �������ݣ���ֹ ring buffer ���
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
 * @brief �մ�������ݣ�MCP��ڣ���������
 */
void CuckooStateMachine::StartLindaShow() {
    // MCP ������ڣ��������ء�ʵ�ʱ����ں�̨����ִ�С�
    if (is_running_) return;  // ���б��������У��ܾ�
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->LindaShow();
        vTaskDelete(nullptr);
    }, "linda_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief ԰�ӳ������ݣ�MCP��ڣ���������
 */
void CuckooStateMachine::StartGardenShow() {
    // MCP ������ڣ��������ء�ʵ�ʱ����ں�̨����ִ�С�
    if (is_running_) return;  // ���б��������У��ܾ�
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->GardenShow();
        vTaskDelete(nullptr);
    }, "garden_show", 4096, this, 5, nullptr, 1);
}

/**
 * @brief �մ����ʵ��
 * ���̣�LED����0015.mp3���š��赸�����������+LED������˸��ǰ24�룩��LEDȫ�������ֽ�������л������LED����ָ�
 */
void CuckooStateMachine::LindaShow() {
    if (is_running_) { ESP_LOGW(TAG, "LindaShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

    // ����� AI ˵�������ݺ�����ͬʱ���У���ֵ���ӿ��������

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");

    // 1. ��LEDһ��������ʾ���ݼ�����ʼ��
    gpio_set_level(LED_A_GPIO, 1);  // LED_A ��
    gpio_set_level(LED_B_GPIO, 1);  // LED_B ��
    vTaskDelay(pdMS_TO_TICKS(500));  // ��0.5��

    // 2. ����������bg audio������TTS��I2S��
    if (mp3_) { mp3_->SetDisableDucking(true); int song = LINDA_SONG_BASE + (linda_song_counter_++ % LINDA_SONG_COUNT); ESP_LOGI(TAG, "LindaShow: playing %04d.mp3 (counter=%d)", song, (int)linda_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }

    // Wait for bg audio to start draining
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED������˸ + �赸�������ת�����������ֽ���
    //    ǰ24�룺LED������˸ + �赸
    //    24���LEDȫ�����赸����ֱ�����ֽ���
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long m1_timer = music_start_ms;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;
    m1_fwd_time_ = 0;  // ��ת�ۼ�ʱ��
    m1_rev_time_ = 0;  // ��ת�ۼ�ʱ��
    m1_stage_start_ = music_start_ms;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s safety timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        // ����ֹͣǰԼ2�루24��ʱ����LEDȫ������ֹͣ��˸
        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

        // �赸����������1~3�룬������Ȼ��
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
        // LED������˸������ǰ24�룬ÿ6֡�л�һ�� = Լ300ms�����
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

    // 5. ���ֽ��� �� �赸ͣ �� ���Ÿ�л���� �� LEDϨ��
    if (m1_) m1_->Stop();  // ͣ�赸���

    // ���ڽǶȲ�����279��/s
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

    // ��л�����ֻ����ţ�0~4ѭ����
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "LindaShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

    gpio_set_level(LED_A_GPIO, 0);  // LED_A ��
    gpio_set_level(LED_B_GPIO, 0);  // LED_B ��

    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // ��bg audio
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // ��bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);  // �ָ� duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "LindaShow: done");
}

/**
 * @brief ԰�ӱ���ʵ��
 * ���̣�LED����0016.mp3���š�С���ٶ��0~180�Ȱڶ�+LED������˸��ǰ24�룩��LEDȫ�������ֽ����������λ����л������LED����ָ�
 */
void CuckooStateMachine::GardenShow() {
    if (is_running_) { ESP_LOGW(TAG, "GardenShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

    // ����� AI ˵�������ݺ�����ͬʱ���У���ֵ���ӿ��������

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");

    // 1. ��LEDһ��������ʾ���ݼ�����ʼ��
    gpio_set_level(LED_A_GPIO, 1);  // LED_A ��
    gpio_set_level(LED_B_GPIO, 1);  // LED_B ��
    vTaskDelay(pdMS_TO_TICKS(500));  // ��0.5��

    // 2. ����������bg audio������TTS��I2S��
    if (mp3_) { mp3_->SetDisableDucking(true); int song = GARDEN_SONG_BASE + (garden_song_counter_++ % GARDEN_SONG_COUNT); ESP_LOGI(TAG, "GardenShow: playing %04d.mp3 (counter=%d)", song, (int)garden_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 4096, this, 4, nullptr); }

    // Wait for bg audio to start draining
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED��˸ + С���ٶ����ֱ�����ֽ���
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

        // ���������: ÿ50ms֡�ƶ�9��, 0~140�����Ұڶ�
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

    // 5. ���ֽ��� �� С����ƽ������ �� ���Ÿ�л���� �� LEDϨ��
    if (violin_servo_) {
        violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000);  // �ӵ�ǰ�Ƕ�ƽ���ص�90��
    }

    // ��л�����ֻ����ţ�0~4ѭ����
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "GardenShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

    gpio_set_level(LED_A_GPIO, 0);  // LED_A ��
    gpio_set_level(LED_B_GPIO, 0);  // LED_B ��

    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // ��bg audio
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // ��bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);  // �ָ� duck
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

    // M1�赸���: ��ת���1-3�� �� ͣ20ms �� ��ת���1-3�� �� ͣ20ms �� ѭ��������8��
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

        // С���ٶ��: 90��0��180��90 ѭ���ڶ�
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
// ����ٶȲ��ԣ��赸�����תָ������
// ����: cuckoo.motor_test (seconds: 1~60)
// ============================================
void CuckooStateMachine::MotorTest(int seconds) {
    if (is_running_) return;
    is_running_ = true;
    MotorPowerOn();
    ESP_LOGI(TAG, "MotorTest: M1 forward %d seconds...", seconds);
    if (m1_) m1_->Forward(100);  // GPIOֱ��ȫ��ѹ
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    if (m1_) m1_->Stop();
    MotorPowerOff();
    is_running_ = false;
    ESP_LOGI(TAG, "MotorTest: done");
}


/**
 * @brief �ۺϱ�����ڣ�MCP cuckoo.start_show����������
 * ��Core 1����cuckoo_show���� �� StartShowTask
 */
void CuckooStateMachine::StartShow() {
    // ����/��㱨ʱ������������ ������ Show ���AI �������
    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }
    // ��������������壩������ͣ�ٿ�ʼ
    if (is_running_) {
        StopAll();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ���ռ�ã���ֹ�ظ�����
    is_running_ = true;
    current_performance_ = kPerformanceManual;

    // �첽ִ�У�MCP ���ߵ���������ѭ���߳��ϣ�app.Schedule����
    // �������ѭ���������ȴ� AbortSpeaking/TTS-STOP��������
    // ��Schedule �ص��Ų��϶ӣ�TTS-STOP ��״̬ת���޷�ִ�У���
    // ���԰��������������ŵ� Core 1 ��̨�����
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
 * @brief �ۺϱ��ݺ�̨����
 * ���AI������������LED+ˮ�������ѡ���֡�����+С������(�첽����)���赸ѭ����LED������š��ָ�
 */
void CuckooStateMachine::StartShowTask() {
    auto& app0 = Application::GetInstance();
    // Start immediately — ShowTask now uses PlayShowMusicBg (bg audio ring buffer)
    // which the audio service mixes with TTS via ducking, no I2S conflict.
    ESP_LOGI(TAG, "Show: starting immediately (bg audio + ducking handles overlap)");

    app0.GetAudioService().SetWakeWordThreshold(0.99f);

MotorPowerOn();
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

    ShowTask(this);
}

/**
 * @brief �ۺϱ���ִ�У�ShowTask��ڵ��õ�ʵ���߼���
 * ��PerformanceTask����RunDanceIntro/RunDanceLoop/RunDanceFinale
 */
void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Show task started");

    auto& app = Application::GetInstance();

    // ����� AI�����ݺ�����ͬʱ����
    if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // �������ֲ�����

    // ====== Show start: LEDs on + water wheel ======
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // ˮ�� IN1=HIGH

    // ѡ����?
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

    // �첽��������+С���������������赸ѭ��
    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);

    // �赸+С�������̿�ʼ���Ϳ���/С���������У�
    sm->RunDanceLoop();

    // ���ֽ������ָ����Ѵʺ���������
    // ע�⣺StartShow �� AbortSpeaking ������״̬���� listening���� idle����
    // ֱ�ӻָ����������ᵼ�»��Ѵ��� listening ״̬���� AbortSpeaking �� audio_input ����


    if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // �ָ� duck

    // ȷ���豸�ص� idle �ٻָ���������
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    // Clean up bg audio to ensure get_status reports false after show
    app.GetAudioService().EnableBgAudioDrain(false);
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "Show: music ended, wake word threshold will be restored by clock task");

    sm->RunDanceFinale();
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;

    // ====== Stop water wheel + LEDs off ======
    if (sm->water_bird_) sm->water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    sm->MotorPowerOff();

    // �ָ����Ѵ���ֵ���豸����idle���ӿز�����ת����
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
 * @brief ����ֹͣ���е���ͱ���
 * - ͣ���е����M1~M4 + С���� + ˮ����
 * - �������90��
 * - LEDϨ��
 * - �������ֲ�ͣ�����ǲ��Ǳ���ģʽ��
 * - ͨ��Schedule�첽�����豸״̬ΪIdle������������
 */
void CuckooStateMachine::StopAll() {
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
    if (m1_) m1_->Stop();  // �赸
    if (m2_) m2_->Stop();  // ����
    if (m3_) m3_->Stop();  // С��
    if (m4_) m4_->Stop();  // ����
    if (violin_motor_) violin_motor_->Stop();  // С����
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    // ���� stop_all+play_url �����طŴ������
    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
            mp3_->Stop();
        }
    }

    // ��ֱ�� AbortSpeaking TTS ����������������������Ȼ����
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
 * @brief ֹͣ���ֲ��ţ����������Ƶ��
 */
void CuckooStateMachine::StopMusic() {
    if (mp3_) {
        mp3_->Stop();
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

/**
 * @brief ���Ŵ򿪣�M2�����ת��ʱ�� MAIN_DOOR_TIME_MS��
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
 * @brief ���Źرգ�M2�����ת��ʱ�� MAIN_DOOR_TIME_MS��
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
 * @brief С�����һ����Ծ�������ͨ��500ms + �ϵ���ȴ400ms��
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
 * @brief С���������Ծ��AI˵��ʱ���滰�����ࣩ
 * ����50-200ms������ȣ���ȴ300-700ms������
 */
void CuckooStateMachine::BirdJumpShort() {
    // AI˵��ʱС����滰��������Ծ��������ģʽ��
    // ����������50-200msģ����Ȼ��Ծ��
    // ����������ȴ300-700ms������������̫��
    MotorPowerOn();
    if (water_bird_) water_bird_->Stop();  // ��ͣˮ�����ͷ�GPIO39�����ͻ
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // �����ͨ�磬С�񵯳�
    int pulse = 50 + (esp_random() % 151);  // ���50-200ms�������
    vTaskDelay(pdMS_TO_TICKS(pulse));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // ������ϵ磬С�����
    int cooldown = 300 + (esp_random() % 401);  // ���300-700ms��ȴʱ��
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

void CuckooStateMachine::MusicDanceTick() {
    // Mp3Player tracks playback state (stays true during AI speech ducking),
    // while IsBgAudioActive() may briefly drop when audio service clears buffers.
    // Using both ensures music dance survives AI conversations.
    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();
    // Track kids_active_ transition for mid-music changes
    static bool was_kids_active = true;
    bool kids_now = kids_active_.load();

    if (music_playing && !IsRunning()) {
        if (music_dance_enabled_ != 1) {
            music_dance_phase_ = 0;
            music_dance_enabled_ = 1;
            // Music just started: dog comes out (no bark) — only if kids active
            dog_outro_done_ = false;
            if (kids_now) {
                xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogIntro();
                    vTaskDelete(nullptr);
                }, "dog_intro", 4096, this, 5, nullptr, 1);
            }
            was_kids_active = kids_now;
        }

        // Mid-music transition: kids became active → bring dog out
        if (!was_kids_active && kids_now) {
            was_kids_active = true;
            dog_outro_done_ = false;
            xTaskCreatePinnedToCore([](void* arg) {
                ((CuckooStateMachine*)arg)->MusicDogIntro();
                vTaskDelete(nullptr);
            }, "dog_intro", 4096, this, 5, nullptr, 1);
        }
        // Mid-music transition: kids became resting → stop & put dog away
        if (was_kids_active && !kids_now) {
            was_kids_active = false;
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

        // Wait for dog_intro to finish before taking over servo/motor
        if (kids_now && dog_intro_done_) {
            // Dance motor: brief pulse on phase 0 (fwd) and 4 (rev)
            if (phase == 0 && m1_) {
                m1_->Forward();
                vTaskDelay(pdMS_TO_TICKS(70));
                m1_->Stop();
                m1_music_fwd_count_++;
            } else if (phase == 4 && m1_) {
                m1_->Reverse();
                vTaskDelay(pdMS_TO_TICKS(87));
                m1_->Stop();
                m1_music_rev_count_++;
            }
            // Guitar + Dog servos: sync to same phase rhythm
            // Phase 0-3: forward beat, Phase 4-7: backward beat
            static int guitar_angle = 90;
            static int dog_angle = 35;
            bool forward_beat = (phase <= 3);
            int guitar_target = forward_beat ? 110 : 70;   // +/-20
            int dog_target = forward_beat ? 35 : 10;         // dog wags 10-35

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
        was_kids_active = kids_now;
        if (music_dance_enabled_ == 1) {
            // Balance M1 motor: reverse must match forward
            if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
                int diff = m1_music_fwd_count_ - m1_music_rev_count_;
                ESP_LOGI(TAG, "MusicDance: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
                for (int i = 0; i < diff; i++) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(78));
                    m1_->Stop();
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            m1_music_fwd_count_ = 0;
            m1_music_rev_count_ = 0;
            if (m1_) m1_->Stop();
            if (violin_servo_) violin_servo_->SetAngle(90);
            // Music ended: dog goes back, close door (only if kids were active)
            if (!dog_outro_done_) {
                dog_outro_done_ = true;
                xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 5, nullptr, 1);
            }
        }
        dog_intro_done_ = false;
        music_dance_enabled_ = 0;
    }
}

void CuckooStateMachine::MusicDogIntro() {
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open_m", 2048, ctx, 5, nullptr, 1);
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
    dog_intro_done_ = true;
    ESP_LOGI(TAG, "MusicDogIntro: done, handing over to MusicDanceTick");
}

void CuckooStateMachine::MusicDogOutro() {
    // First smooth return to 30deg, then back to home
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
}

void CuckooStateMachine::KidsComeOut() {
    kids_active_ = true;
    SaveKidsActive();
    ESP_LOGI(TAG, "KidsComeOut: kids active, will dance with music");
}

void CuckooStateMachine::KidsRest() {
    kids_active_ = false;
    SaveKidsActive();
    // Balance M1 motor: reverse must match forward before stopping
    if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
        int diff = m1_music_fwd_count_ - m1_music_rev_count_;
        ESP_LOGI(TAG, "KidsRest: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
        for (int i = 0; i < diff; i++) {
            m1_->Reverse();
            vTaskDelay(pdMS_TO_TICKS(78));
            m1_->Stop();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    m1_music_fwd_count_ = 0;
    m1_music_rev_count_ = 0;
    // Stop all movement
    if (m1_) m1_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    if (dog_servo_) dog_servo_->SetAngle(dog_state_.angle);  // hold current position
    ESP_LOGI(TAG, "KidsRest: kids resting, no movement");
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

/**
 * @brief ���ö���Ƕ�
 * @param servo_id 0=С���ٶ��, 1=С��β�Ͷ��
 * @param angle 0~180��
 */
void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
    MotorPowerOn();  // ȷ����Դ�ȶ�
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

/**
 * @brief ���õ���ٶ�
 * @param motor_id 1=M1�赸, 2=С���ٵ��, 3=С�����, 4=���ŵ��
 * @param speed -100~100 (����=��ת)
 */
void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
        case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;  // С���ٵ�� (��B M2, GPIO18/45)
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
 * @brief �������ֲ������
 * @param url_or_path URL�����·������ "/pcm?q=�ܽ���"��
 * @return 0=�ɹ�, <0=ʧ��
 * - ���AI�Ի�����CPU����
 * - �Զ�����URL�е����ĺͿո�
 * - ���ļ�������httpǰ׺���Զ�ƴ�Ӵ�����ַ
 * - ���ڲ���ʱ��ͣ�ɸ��ٷ��¸�
 */
std::string CuckooStateMachine::CantoneseLookup(const char* word) {
    if (music_proxy_host_.empty()) {
        ESP_LOGW(TAG, "CantoneseLookup: proxy not configured");
        return "";
    }

    // URL-encode the word manually for esp_http_client
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

    // Read response body
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

std::string CuckooStateMachine::CheckMultiArtist(const char* url_or_path) {
    if (music_proxy_host_.empty()) return "";

    // 第1步：URL编码 —— 把中文字符转成 %XX 格式
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

    // 第2步：修正路径 —— /opus 或 /pcms 统一改成 /pcm
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

int CuckooStateMachine::PlayOnlineMusic(const char* url_or_path) {
    if (!mp3_) return -1;
    
 // ����� AI ˵������ TTS ������ͬʱ���У�PlayOpus ���Զ� duck �� 30%��
    auto& app = Application::GetInstance();
    auto ai_state = app.GetDeviceState();
    if (ai_state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "PlayOnlineMusic: AI speaking, music will overlap (ducked)");
    }
    
    // If already playing, stop old task cleanly to start new one.
    
    // Auto-detect format from URL path
    // ���������URL��http��ͷ����ֱ��ʹ��
    if (strncmp(url_or_path, "http", 4) == 0) {
        char conv_url[1280];
        strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
        conv_url[sizeof(conv_url) - 1] = '\0';
        // ���� URL �еĿո�Ϊ %20
        for (char* p = conv_url; *p; p++) {
            if (*p == ' ') {
                // ���� 2 �ַ�
                size_t tail_len = strlen(p + 1);
                if ((size_t)(p + 3 + tail_len - conv_url) >= sizeof(conv_url)) break;
                memmove(p + 3, p + 1, tail_len + 1);
                p[0] = '%'; p[1] = '2'; p[2] = '0';
            }
        }
        ConvertToPcmUrl(conv_url, sizeof(conv_url));
        // ������ڲ��ţ���ͣ�ɸ��ٷ��¸�
        if (mp3_->IsPlaying()) {
            ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
            mp3_->Stop();
            vTaskDelay(pdMS_TO_TICKS(200));
            // �Ⱦ�PlayOpusTask�˳���is_playing_��false��
            int wait = 0;
            while (mp3_->IsPlaying() && wait < 100) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            // ��ձ�����Ƶbuffer��ֹǰ�����׻���
            Application::GetInstance().GetAudioService().ClearBackgroundAudio();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return mp3_->PlayOpus(conv_url);
    }
    
    // �����ô�����ַƴ��
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
            // �ո��� HTTP request line ���Ƿָ������������
            if (dst + 3 <= end) {
                *dst++ = '%';
                *dst++ = '2';
                *dst++ = '0';
            }
            src++;
            continue;
        }
        if (c < 0x80) {
        // ASCIIֱ�Ӹ���
            *dst++ = *src;
        } else {
        // ��ASCII %XX ����
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
    // ������ڲ��ţ���ͣ�ɸ��ٷ��¸�
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
 * @brief �������ִ�����������ַ
 * @param host ��������IP
 * @param port �����˿�
 */
void CuckooStateMachine::SetMusicProxy(const char* host, int port) {
    music_proxy_host_ = host;
    music_proxy_port_ = port;
    ESP_LOGI(TAG, "Music proxy set to %s:%d", host, port);
}

// ============================================
// CuckooTools MCPע��
// ============================================
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

void CuckooTools::RegisterAll() {