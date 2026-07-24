// ===== Part 4: 演出/报时/闹钟/狗秀/琳达/花园 (L2395-4412) =====
/**
 * @brief 启动表演（整点/半点/手动触发）
 *
     // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
 * @param type 表演类型：kPerformanceHour(整点)/kPerformanceHalf(半点)/kPerformanceManual(手动)
 * @param hour 小时（半点和手动报时不需此参数）
 *
     // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断）----
 * - AI 触发后 5 秒内忽略重复触发
 * - 先用 AbortSpeaking 终止 TTS，防止冲突
 * - 在 Core 1 上创建 PerformanceTask 执行
 */
void CuckooStateMachine::StartPerformance(PerformanceType type, int hour) {
        // 防止重入：已有表演在进行中
    if (is_running_) { ESP_LOGW(TAG, "Already performing"); return; }

    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----


        // 检查距上次idle退出是否不足5秒（5000000us）
    if (last_idle_exit_us_ > 0) {
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - last_idle_exit_us_) < 5000000) {
            // 如果在5秒窗口内 → 拒绝（AI可能还在处理中）
            ESP_LOGW(TAG, "StartPerformance blocked: within 5s of wake-up (likely AI auto-trigger)");
            return;
        }
    }


    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
        // ---- 等待AI完成当前话语/监听，防止表演与TTS冲突 ----
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
            // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断）----
        ESP_LOGI(TAG, "Aborting AI speaking before performance (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
            // 等待200ms确保AI完全转入idle状态
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    app.GetAudioService().EnableVoiceProcessing(false);

    app.GetAudioService().EnableWakeWordDetection(false);

    is_running_ = true;
    current_performance_ = type;
    switch (type) {
                case kPerformanceHour:  // === 整点报时 ===
        case kPerformanceHour:
            if (hour > 12) hour -= 12;
            if (hour <= 0) hour = 12;
            total_calls_ = hour;
            call_count_ = 3;
            break;
            // ====== 半点报时：3声钟鸣 + 舞蹈 ======
                case kPerformanceHalf:  // === 半点报时 ===
        case kPerformanceHalf:
            total_calls_ = 0;
            call_count_ = 3;
            break;
            // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
                case kPerformanceManual:  // === 手动触发 ===
        case kPerformanceManual:
            total_calls_ = call_count_ = 3;
            break;
            // 重置运行标志，允许下次表演触发
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




 */
/**
void CuckooStateMachine::PerformanceTask(void* arg) {
 * @brief 表演任务（在Core 1执行）——整点/半点/手动统一入口
    auto* sm = static_cast<CuckooStateMachine*>(arg);
 *
    sm->violin_state_.Reset();
 * 整点：报时N声 + 0013.mp3循环N次 + 舞蹈(LED+水车+跳舞电机+小提琴+小狗)
    sm->dog_state_.Reset();
 * 半点：报时3声，无音乐仅舞蹈
    ESP_LOGI(TAG, "Performance task started");
 * 手动：指定唤醒词触发后播放音乐 + 舞蹈

 */

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);


    if (sm->current_performance_ == kPerformanceHour) {

                    // Phase 1: 大门开 + 小鸟开门 + 报时N声 + 大门关
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



        app.GetAudioService().SetOutputMuted(false);
            // 控制LED灯（A和B交替闪烁）
        gpio_set_level(LED_A_GPIO, 1);
        gpio_set_level(LED_B_GPIO, 1);
            // 水车旋转（连续水流效果）
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // Phase 2: 音乐 + 舞蹈（bg audio，与 start_show/LindaShow/GardenShow 同路径）
        if (sm->mp3_ && sm->total_calls_ >= 1 && sm->total_calls_ <= 12) {
            sm->mp3_->SetDisableDucking(true);
            int song = sm->total_calls_;
            sm->show_music_index_ = song;
            xTaskCreatePinnedToCore([](void* arg) {
                auto* s = (CuckooStateMachine*)arg;
                    // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
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


                // 舞蹈循环：M1电机+小提琴+小狗摇尾+LED闪烁（音乐播放期间持续）
            sm->RunDanceLoop();

            // 开场（开门+小狗，异步，~5s）必须在收尾前完成
            // 否则小狗回退/关门会与开场重叠
            {
                unsigned long since_intro = xTaskGetTickCount() * portTICK_PERIOD_MS - intro_start_ms;
                if (since_intro < 6000) vTaskDelay(pdMS_TO_TICKS(6000 - since_intro));
            }
                // 舞蹈收尾：电机归位+小提琴平衡+小狗回退+大门关闭
            sm->RunDanceFinale();
        }

    // 关闭水车 + 灯光熄灭
        if (sm->water_bird_) sm->water_bird_->Stop();
            // 控制LED灯（A和B交替闪烁）
        gpio_set_level(LED_A_GPIO, 0);
        gpio_set_level(LED_B_GPIO, 0);


        // ====== 半点报时：3声钟鸣 + 舞蹈 ======
    } else if (sm->current_performance_ == kPerformanceHalf) {
                    // Phase 1: 大门开 + 小鸟开门 + 报时N声 + 大门关
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
        // 表演结束，电机断电
    sm->MotorPowerOff();


    if (app.GetDeviceState() == kDeviceStateIdle) {
            // 表演结束后恢复唤醒词检测（阈值0.02=高灵敏度）
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "Performance complete");

    vTaskDelete(NULL);
}
/**





 */
void CuckooStateMachine::CheckTime(int hour, int min, bool dark) {
    is_dark_ = dark;  // 更新环境光线状态
    // ---- 前置条件检查：表演中 / 设备忙 / 静音模式 ----
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

    // ---- 静音模式判断（4档）----
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
        // 模式3: 时间段静音（start~end区间内静音，支持跨夜）
        int start = quiet_start_.load();
        int end = quiet_end_.load();
        if (start < end) {
            if (hour >= start && hour < end) {
                ESP_LOGI(TAG, "Skipping chime: quiet_mode=3 window %d-%d", start, end);
                return;
            }
        } else {
            // 跨夜区间（如22:00~6:00）：hour>=start 或 hour<end 都在区间内
            if (hour >= start || hour < end) {
                ESP_LOGI(TAG, "Skipping chime: quiet_mode=3 overnight window %d-%d", start, end);
                return;
            }
        }
    }





    if (min == 0) {
        ESP_LOGI(TAG, "Hourly chime: %d:%02d, starting performance", hour, min);
        if (hourly_perf_.load()) { StartPerformance(kPerformanceHour, hour); } else { ESP_LOGI(TAG, "Hourly chime: performance disabled"); }
        MarkHourlyChime(hour);  // 触发成功后去重，防止重复整点报时
    }

    else if (min == 30) {
        ESP_LOGI(TAG, "Half-hour chime: %d:%02d, starting mini performance", hour, min);
            // ====== 半点报时：3声钟鸣 + 舞蹈 ======
        StartPerformance(kPerformanceHalf, hour);
        MarkHalfHourlyChime(hour);  // 触发成功后去重，防止重复半点报时
    }
}

/**
 * @brief 设置当前时间
 *
 * @param hour 小时
 * @param min 分钟
 * @param sec 秒
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

/**
 * @brief 从 NVS 加载静音模式配置
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
 * @brief 从 NVS 加载 Kids 模式状态
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

 * @param hour Сʱ (0-23)



 */
void CuckooStateMachine::SetAlarm(int hour, int minute, bool repeat_daily) {
        // 加锁保护闹钟数组的并发访问
    std::lock_guard<std::mutex> lock(alarm_mutex_);
    if (alarm_count_ >= kMaxAlarms) {
        ESP_LOGW(TAG, "Alarm list full (max %d)", kMaxAlarms);
        return;
    }
 // 检查重复闹钟 - 更新已有闹钟
    for (int i = 0; i < alarm_count_; i++) {
            // 小时和分钟都匹配且到达整秒 → 触发
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


 */
std::string CuckooStateMachine::GetAlarmsJson() {
        // 加锁保护闹钟数组的并发访问
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
 * @param index 闹钟索引（0=第一个）
 */
bool CuckooStateMachine::DeleteAlarm(int index) {
        // 加锁保护闹钟数组的并发访问
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


 */
void CuckooStateMachine::StopAlarm() {
    alarm_stopped_ = true;
    alarm_ringing_ = false;
 // 一次性闹钟，用户停止后禁用
    {
            // 加锁保护闹钟数组的并发访问
        std::lock_guard<std::mutex> lock(alarm_mutex_);
        for (int i = 0; i < alarm_count_; i++) {
                // 一次性闹钟：触发后立即禁用
            if (alarms_[i].enabled && !alarms_[i].repeat_daily
                && alarms_[i].hour == current_hour_
                && alarms_[i].minute == current_min_) {
                alarms_[i].enabled = false;
                ESP_LOGI(TAG, "One-shot alarm %02d:%02d disabled after stop",
                         alarms_[i].hour, alarms_[i].minute);
            }
        }
    }
 // 停止所有当前播放的音频
        // 停止MP3播放
    if (mp3_) mp3_->Stop();
    ESP_LOGI(TAG, "Alarm stopped by user");
}

/**




 */
void CuckooStateMachine::CheckAlarms(int hour, int minute, int sec) {
    if (alarm_ringing_) return;  // 已有闹钟在响，跳过
    if (!time_set_) return;      // 时间尚未设置，无法触发闹钟

    {
            // 加锁保护闹钟数组的并发访问
        std::lock_guard<std::mutex> lock(alarm_mutex_);
        for (int i = 0; i < alarm_count_; i++) {
                // 找到第一个未启用的闹钟槽位
            if (!alarms_[i].enabled) continue;
                // 小时和分钟都匹配且到达整秒 → 触发
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
                break;  // 同一时间只触发一个闹钟
            }
        }
    }
}

/**


 */
void CuckooStateMachine::AlarmTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    ESP_LOGI(TAG, "Alarm task started");

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    while (sm->alarm_ringing_ && !sm->alarm_stopped_) {
 // 播放闹铃循环50次（2s/次 = 总计100s）
 // 第一轮：前10次音量从15%渐升至100%（20s内）
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

 // 贪睡：等待2分钟，每秒检查 stopped_
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


 */
/**
void CuckooStateMachine::DogShow() {
 * @brief 小狗秀：小狗出门→叫一声→摇尾→叫一声→回退→关门
    if (is_running_) return;
 * 狗叫叠加混音到背景音乐上（不打断），若无音乐则OutputRawPcm直出

 */
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);
    }, "dog_show", 4096, this, 5, nullptr, 1);
}

/**


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
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----

    // 步骤1: 启动水车
        // 水车旋转（连续水流效果）
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 步骤2: 打开大门
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
            // 延时等待门动作完成
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }


    if (dog_servo_) {
                    // 异步推门+叫：狗尾巴扫出 + 前进 + 叫一声 + 复位
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
            // 小狗尾巴微调（准备/归位）
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
            // 小狗尾巴微调（准备/归位）
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
            // 延时等待门动作完成
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 清理资源
        // 表演结束，电机断电
    MotorPowerOff();
        // 重置运行标志，允许下次表演触发
    is_running_ = false;
    current_performance_ = kPerformanceNone;

            // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断）----
    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈值0.02=高灵敏度）
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "DogShow: done");
}

/**




 */
void CuckooStateMachine::PlayWavAsset(const char* filename) {
    void* wav_ptr = nullptr;
    size_t wav_size = 0;
    if (!Assets::GetInstance().GetAssetData(filename, wav_ptr, wav_size)) {
        ESP_LOGW(TAG, "PlayWavAsset: %s not found", filename);
        return;
    }
        // WAV文件头至少44字节
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
 * @brief 直接播放狗叫 WAV（不经过混音）
 */
void CuckooStateMachine::PlayDogBarkDirect() {
    PlayWavAsset("dog_bark.wav");
}

    // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
/**
 * @brief 异步播放背景音乐（走bg audio环形缓冲，不阻塞）
 *
 * @param index MP3文件编号
 */
bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;


    int16_t* pcm = nullptr;
    size_t total_samples = 0;
    int src_sr = 0;
    if (mp3_->DecodeToBuffer(index, &pcm, &total_samples, &src_sr) < 0) {
            // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
        ESP_LOGE(TAG, "PlayShowMusicBg: decode failed for %04d.mp3", index);
        return false;
    }

        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 %u samples %dHz", index, (unsigned)total_samples, src_sr);

    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetBackgroundAudioGain(1.0f);

    const int DST_SR = 16000;
    const size_t CHUNK_SRC = 2048;  // 分小块推送
    size_t offset = 0;

    while (offset < total_samples && is_running_) {
        size_t chunk = (total_samples - offset > CHUNK_SRC) ? CHUNK_SRC : (total_samples - offset);


        if (src_sr != DST_SR) {
            float ratio = (float)src_sr / DST_SR;
            std::vector<int16_t> dst(chunk * 2 / 3 + 2);  // 重采样后体积缩小约30%
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
            // 等待200ms确保AI完全转入idle状态
        vTaskDelay(pdMS_TO_TICKS(200));
            // 清空残留背景音频，防止前一次Opus/PCM残留干扰
        audio.ClearBackgroundAudio();
            // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
        ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 finished, bg audio cleared", index);
    }
    return true;
}

/**

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


 */
/**
void CuckooStateMachine::LindaShow() {
 * @brief 琳达秀：播放0015.mp3 + 舞蹈开场/循环/收尾
    if (is_running_) { ESP_LOGW(TAG, "LindaShow: already running, skip"); return; }
 */

    auto& app = Application::GetInstance();



    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");


        // 控制LED灯（A和B交替闪烁）
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));


        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
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
                // 控制LED灯（A和B交替闪烁）
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
                    // 停止所有电机
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
                    // 停止所有电机
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
                    // 控制LED灯（A和B交替闪烁）
                gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
                gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }


        // 停止所有电机
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
                // 停止所有电机
            m1_->Stop();
        }
    }


    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "LindaShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

        // 控制LED灯（A和B交替闪烁）
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

        // 停止MP3播放
    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
        // 表演结束，电机断电
    MotorPowerOff();
        // 重置运行标志，允许下次表演触发
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);
    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈值0.02=高灵敏度）
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "LindaShow: done");
}

/**


 */
/**
void CuckooStateMachine::GardenShow() {
 * @brief 花园秀：播放0016.mp3 + 舞蹈
    if (is_running_) { ESP_LOGW(TAG, "GardenShow: already running, skip"); return; }
 */

    auto& app = Application::GetInstance();



    is_running_ = true;
        // ====== 手动表演（AI唤醒触发）：播放音乐 + 舞蹈 ======
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");


        // 控制LED灯（A和B交替闪烁）
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));


        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
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
                // 控制LED灯（A和B交替闪烁）
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
                    // 控制LED灯（A和B交替闪烁）
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

        // 控制LED灯（A和B交替闪烁）
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

        // 停止MP3播放
    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);
        // 表演结束，电机断电
    MotorPowerOff();
        // 重置运行标志，允许下次表演触发
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);
    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈值0.02=高灵敏度）
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "GardenShow: done");
}

/**
 * @brief 舞蹈全套：开场→循环→收尾→断电
 */
void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
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
                    // 停止所有电机
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
                    // 停止所有电机
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

        // 停止所有电机
    if (m1_) m1_->Stop();
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
        // 重置运行标志，允许下次表演触发
    is_running_ = false;
        // 表演结束，电机断电
    MotorPowerOff();
    ESP_LOGI(TAG, "Dance finished");
}

// ============================================


// ============================================
/**
 * @brief 电机测试：所有电机顺序运行指定秒数
 *
 * @param seconds 运行秒数
 */
void CuckooStateMachine::MotorTest(int seconds) {
    if (is_running_) return;
    is_running_ = true;
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    ESP_LOGI(TAG, "MotorTest: M1 forward %d seconds...", seconds);
    if (m1_) m1_->Forward(100);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
        // 停止所有电机
    if (m1_) m1_->Stop();
        // 表演结束，电机断电
    MotorPowerOff();
        // 重置运行标志，允许下次表演触发
    is_running_ = false;
    ESP_LOGI(TAG, "MotorTest: done");
}


/**


 */
void CuckooStateMachine::StartShow() {

    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }

    if (is_running_) {
        StopAll();
            // 等待200ms确保AI完全转入idle状态
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


 */
void CuckooStateMachine::StartShowTask() {
    auto& app0 = Application::GetInstance();
        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）


    ESP_LOGI(TAG, "Show: starting immediately (bg audio + ducking handles overlap)");

    app0.GetAudioService().SetWakeWordThreshold(0.99f);

MotorPowerOn();
    // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

    ShowTask(this);
}

/**


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
        // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）


    if (sm->mp3_) {
        sm->mp3_->SetDisableDucking(true);
        xTaskCreatePinnedToCore([](void* arg) {
            auto* s = (CuckooStateMachine*)arg;
                // 异步加载并播放背景音乐（ShowMusicBg走bg audio环形缓冲）
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


    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
            // 舞蹈开场：小狗出来
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);


        // 舞蹈循环：M1电机+小提琴+小狗摇尾+LED闪烁（音乐播放期间持续）
    sm->RunDanceLoop();






    if (sm->mp3_) sm->mp3_->SetDisableDucking(false);


    if (app.GetDeviceState() != kDeviceStateIdle) {
            // ---- 强制终止TTS + 禁用唤醒词检测（表演期间不被打断）----
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    app.GetAudioService().EnableBgAudioDrain(false);
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "Show: music ended, wake word threshold will be restored by clock task");

        // 舞蹈收尾：电机归位+小提琴平衡+小狗回退+大门关闭
    sm->RunDanceFinale();
        // 重置运行标志，允许下次表演触发
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;


    if (sm->water_bird_) sm->water_bird_->Stop();
        // 控制LED灯（A和B交替闪烁）
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

        // 表演结束，电机断电
    sm->MotorPowerOff();




    if (app.GetDeviceState() == kDeviceStateIdle)
            // 表演结束后恢复唤醒词检测（阈值0.02=高灵敏度）
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    else
        app.GetAudioService().SetWakeWordThreshold(0.30f);

    ESP_LOGI(TAG, "Show task finished");
    vTaskDelete(NULL);
}

/**
 * @brief 播放指定编号的 MP3 音乐
 *
 * @param index MP3文件编号
 */
void CuckooStateMachine::PlayMusic(int index) {
    if (mp3_) mp3_->PlayBgMusic(index);
}

/**






 */
void CuckooStateMachine::StopAll() {
        // 重置运行标志，允许下次表演触发
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
        // 停止所有电机
    if (m1_) m1_->Stop();
    if (m2_) m2_->Stop();
    if (m3_) m3_->Stop();
    if (m4_) m4_->Stop();
    if (violin_motor_) violin_motor_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
        // 控制LED灯（A和B交替闪烁）
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
                // 表演结束后恢复唤醒词检测（阈值0.02=高灵敏度）
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
            Application::GetInstance().GetAudioService().RefreshOutputTimestamp();
            Application::GetInstance().GetAudioService().RefreshInputTimestamp();
        });
    }
    ESP_LOGI(TAG, "All motors stopped");
        // 表演结束，电机断电
    MotorPowerOff();
}

/**

 */
    // 停止背景音乐播放
void CuckooStateMachine::StopMusic() {
    if (mp3_) {
            // 停止MP3播放
        mp3_->Stop();
            // 清空残留背景音频，防止前一次Opus/PCM残留干扰
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

/**

 */
    // 大门正向开门
void CuckooStateMachine::OpenDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
            // 延时等待门动作完成
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
        // 表演结束，电机断电
    MotorPowerOff();
}

/**

 */
    // 大门反向关门
void CuckooStateMachine::CloseDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
            // 延时等待门动作完成
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
        // 表演结束，电机断电
    MotorPowerOff();
}

/**

 */
void CuckooStateMachine::BirdJumpOnce() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
        // 表演结束，电机断电
    MotorPowerOff();
}

void CuckooStateMachine::BirdJumpPulse() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
}

/**


 */
void CuckooStateMachine::BirdJumpShort() {



    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    int pulse = 50 + (esp_random() % 151);
    vTaskDelay(pdMS_TO_TICKS(pulse));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    int cooldown = 300 + (esp_random() % 401);
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

/**
 * @brief 音乐舞蹈 tick（250ms 周期）：在线音乐播放期间的小狗/小提琴动作控制
 */
void CuckooStateMachine::MusicDanceTick() {



    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();

    static bool was_kids_active = true;
        // 检查Kids模式是否激活
    bool kids_now = kids_active_.load();

    if (music_playing && !IsRunning()) {
        if (music_dance_enabled_ != 1) {
            music_dance_phase_ = 0;
            music_dance_enabled_ = 1;

            dog_outro_done_ = false;
                // 检查Kids模式是否激活
            if (kids_now) {
                xTaskCreatePinnedToCore([](void* arg) {
                        // 触发小狗出场动作
                    ((CuckooStateMachine*)arg)->MusicDogIntro();
                    vTaskDelete(nullptr);
                }, "dog_intro", 4096, this, 5, nullptr, 1);
            }
                // 检查Kids模式是否激活
            was_kids_active = kids_now;
        }


            // 检查Kids模式是否激活
        if (!was_kids_active && kids_now) {
            was_kids_active = true;
            dog_outro_done_ = false;
            xTaskCreatePinnedToCore([](void* arg) {
                    // 触发小狗出场动作
                ((CuckooStateMachine*)arg)->MusicDogIntro();
                vTaskDelete(nullptr);
            }, "dog_intro", 4096, this, 5, nullptr, 1);
        }

            // 检查Kids模式是否激活
        if (was_kids_active && !kids_now) {
            was_kids_active = false;
                // 停止所有电机
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
                    // 停止所有电机
                m1_->Stop();
                m1_music_fwd_count_++;
            } else if (phase == 4 && m1_) {
                m1_->Reverse();
                vTaskDelay(pdMS_TO_TICKS(87));
                    // 停止所有电机
                m1_->Stop();
                m1_music_rev_count_++;
            }


            static int guitar_angle = 90;
            static int dog_angle = 35;
            bool forward_beat = (phase <= 3);
            int guitar_target = forward_beat ? 110 : 70;   // +/-20
            int dog_target = forward_beat ? 35 : 10;         // 小狗摇尾范围 10~35 度

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
            // 检查Kids模式是否激活
        was_kids_active = kids_now;
        if (music_dance_enabled_ == 1) {

            if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
                int diff = m1_music_fwd_count_ - m1_music_rev_count_;
                ESP_LOGI(TAG, "MusicDance: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
                for (int i = 0; i < diff; i++) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(78));
                        // 停止所有电机
                    m1_->Stop();
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            m1_music_fwd_count_ = 0;
            m1_music_rev_count_ = 0;
                // 停止所有电机
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
 * @brief 在线音乐播放时小狗出场动作
 */
    // 触发小狗出场动作
void CuckooStateMachine::MusicDogIntro() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open_m", 2048, ctx, 5, nullptr, 1);
    if (dog_servo_) {
                    // 异步推门+叫：狗尾巴扫出 + 前进 + 叫一声 + 复位
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
        // 表演结束，电机断电
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
                // 停止所有电机
            m1_->Stop();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    m1_music_fwd_count_ = 0;
    m1_music_rev_count_ = 0;

        // 停止所有电机
    if (m1_) m1_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    if (dog_servo_) dog_servo_->SetAngle(dog_state_.angle);  // 保持当前角度
    ESP_LOGI(TAG, "KidsRest: kids resting, no movement");
}

// 小鸟门开门（m4_电机正向）
/**
 * @brief 小鸟门开门（M4电机正向）
 */
void CuckooStateMachine::OpenBirdDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (m4_) {
        m4_->Forward(BIRD_DOOR_OPEN_SPEED);
            // 延时等待门动作完成
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
        // 表演结束，电机断电
    MotorPowerOff();
}

// 小鸟门关门（m4_电机反向）
/**
 * @brief 小鸟门关门（M4电机反向）
 */
void CuckooStateMachine::CloseBirdDoor() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (m4_) {
        m4_->Reverse(BIRD_DOOR_CLOSE_SPEED);
            // 延时等待门动作完成
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
        // 表演结束，电机断电
    MotorPowerOff();
}

/**
 * @brief 播放布谷鸟叫声
 */
void CuckooStateMachine::PlayCuckooSound() {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (bell_player_) {
            // 停止布谷鸟声
        bell_player_->PlayCuckooSoundSync();
    }
        // 表演结束，电机断电
    MotorPowerOff();
}

/**



 */
void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
    MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

/**



 */
void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
        case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;
        case 3: if (m3_) m3_->SetSpeed(speed); break;
        case 4: if (m4_) m4_->SetSpeed(speed); break;
        default: break;
    }
        // 表演结束，电机断电
    if (speed == 0) MotorPowerOff();
}

/**
 * @brief MCP工具：设置小鸟门电机速度
 *
 * @param speed 速度(-100~100)
 */
void CuckooStateMachine::SetBirdDoorSpeed(int speed) {
    if (speed != 0) MotorPowerOn();
        // ---- 防误触发：AI唤醒后5秒内忽略表演请求（可能是语音误判）----
    if (m4_) m4_->SetSpeed(speed);
        // 表演结束，电机断电
    if (speed == 0) MotorPowerOff();
}

/**







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
 * @param url_or_path 原始URL或路径
 */
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

/**
 * @brief 播放在线音乐（通过QQ音乐代理）
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
            // 将/stream或/opus改写为/pcm降低解码负载
        ConvertToPcmUrl(conv_url, sizeof(conv_url));

        if (mp3_->IsPlaying()) {
            ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
                // 停止MP3播放
            mp3_->Stop();
                // 等待200ms确保AI完全转入idle状态
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
    
    char full_url[1280];  // URL 完整路径: http:// + host + :port + 编码后路径

    if (encoded_path[0] != '/') {
        snprintf(full_url, sizeof(full_url), "http://%s:%d/%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    } else {
        snprintf(full_url, sizeof(full_url), "http://%s:%d%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    }
        // 将/stream或/opus改写为/pcm降低解码负载
    ConvertToPcmUrl(full_url, sizeof(full_url));

    if (mp3_->IsPlaying()) {
        ESP_LOGI(TAG, "PlayOnlineMusic: stopping current music for new request");
            // 停止MP3播放
        mp3_->Stop();
            // 等待200ms确保AI完全转入idle状态
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

// ============================================