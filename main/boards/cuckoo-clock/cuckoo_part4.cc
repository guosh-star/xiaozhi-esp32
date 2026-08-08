/**
 * @brief 小狗秀入口：在 Core 1 后台任务执行完整小狗表演
 * 若已在演出则拒绝（防止重复）；任务内调用 DogShowTask。
 */
void CuckooStateMachine::DogShow() {
    if (is_running_) return;  // 演出正在进行中，拒绝重复请求
    // 后台线程执行表演，固定在 Core 1，避开音频核心
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->DogShowTask();
        vTaskDelete(nullptr);  // 任务完成后删除自身
    }, "dog_show", 8192, this, 5, nullptr, 1);
}

/**
 * @brief 小狗秀任务实现（完整流程）
 * 开门→水车转→狗尾伸出(180→20°)→小狗前进→叫一声→狗尾归0°→
 * 摇尾10秒(0↔60°)→叫一声→狗尾回20°→小狗后退→狗尾归位(20→180°)→停水车→关门
 * 结束时恢复唤醒词阈值（设备 idle 时）。
 */
void CuckooStateMachine::DogShowTask() {
    auto& app = Application::GetInstance();

    // === 防止 AI 说话与狗叫同时播放 ===
    // === 若同时播放会互相干扰，音量被压缩不自然 ===

    // === 第3步：开始演出 ===
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "DogShow: start");

    MotorPowerOn();

    // 1. 开门
    if (m2_) {
        m2_->Forward(MAIN_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 2. 水车开启（开门后启动，避免共享电源轨电压跌落）
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);

    // 3. 狗尾伸出：180→20 度，1200ms
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

    // 5. 叫一声（第1次）
    PlayDogBarkDirect();
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // restart after BirdJumpShort stopped it

    // 6. 狗尾归 0 度
    if (dog_servo_) {
        dog_servo_->Sweep(20, 0, 300);
        dog_state_.angle = 0;
    }

    // 7. 摇尾 10 秒：0↔60 度
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

    // 8. 叫一声（第2次）
    PlayDogBarkDirect();
    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // restart after BirdJumpShort stopped it

    // 9. 狗尾回到 20 度
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

    // 11. 狗尾回到 180 度
    if (dog_servo_) {
        dog_servo_->Sweep(20, 180, 1200);
        dog_state_.angle = 180;
    }

    // 12. 停水车
    if (water_bird_) water_bird_->Stop();

    // 13. 关门
    if (m2_) {
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }

    // 收尾清理
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
 * @brief 通过 WAV 文件播放（同步，阻塞到播放完成）
 * @param filename WAV 文件名（如 dog_bark.wav, 2001.wav~2005.wav）
 * 解析 WAV 头获取采样率和数据块；dog_bark.wav 直出，其他 WAV 增益放大后输出；
 * 输出后 vTaskDelay 等待播放完成（同步）。
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

    // 查找数据块（前面可能有 fmt、LIST 等块）
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
        // DogShow 路径：无背景音乐，直接用 OutputRawPcm 播放
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
 * @brief 直接播放狗叫（不走混音队列），封装 PlayWavAsset
 */
void CuckooStateMachine::PlayDogBarkDirect() {
    PlayWavAsset("dog_bark.wav");
}

/**
 * @brief 播放演出背景音乐（走 bg audio 环形缓冲，异步）
 * @param index MP3 编号
 * 用 DecodeStreaming 逐帧解码→重采样到 16kHz→推入背景音频环形缓冲；
 * 缓冲满 64000 样本后开启排空；播放完清空缓冲。
 * @return true=播放完成；false=解码失败或未初始化
 */
bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;

    auto& audio = Application::GetInstance().GetAudioService();
    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 START (BUILD=2026-08-07-21:25)", index);
    audio.SetBackgroundAudioGain(1.0f);
    audio.EnableBgAudioDrain(true);  // drain 从一开始就开着
    // 注: bg_audio_active_ 由 SetBackgroundAudioGain(gain>0) 自动设置，无需额外调 SetBgAudioActive

    struct StreamCtx {
        AudioService* audio;
        std::atomic<bool>* is_running;
        size_t total_pushed;
        bool drain_enabled;
    } ctx = {&audio, &is_running_, 0, false};

    int ret = mp3_->DecodeStreaming(index, [](const int16_t* pcm, size_t samples, int src_sr, void* user_data) {
        auto* c = (StreamCtx*)user_data;
        size_t offset = 0;
        while (offset < samples && c->is_running->load()) {
            size_t chunk = (samples - offset > 2048) ? 2048 : (samples - offset);
            if (src_sr != 32000) {
                float ratio = (float)src_sr / 32000;
                std::vector<int16_t> dst(chunk * 2 + 4);
                size_t d = 0; float pos = 0;
                while (pos < (float)chunk - 1.0f && d < dst.size() - 1) {
                    size_t idx = (size_t)pos;
                    float frac = pos - idx;
                    dst[d++] = (int16_t)((float)pcm[offset + idx] * (1.0f - frac) + (float)pcm[offset + idx + 1] * frac);
                    pos += ratio;
                }
                c->audio->PushBackgroundAudio(dst.data(), d, 32000);
            } else {
                c->audio->PushBackgroundAudio(pcm + offset, chunk, 32000);
            }
            c->total_pushed += chunk;
            offset += chunk;
            // 快满时暂停解码，防止 ring 溢出
            while (c->audio->GetBgAudioFillLevel() > 96000 && c->is_running->load())
                vTaskDelay(pdMS_TO_TICKS(50));
        }
    }, &ctx);

    if (ret < 0) {
        ESP_LOGE(TAG, "PlayShowMusicBg: decode failed for %04d.mp3", index);
        return false;
    }
    int drain_wait = 0;
    while (is_running_ && audio.GetBgAudioFillLevel() > 0 && drain_wait < 200) {
        vTaskDelay(pdMS_TO_TICKS(50)); drain_wait++;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    audio.ClearBackgroundAudio();
    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 finished, bg audio cleared", index);
    return true;
}

/**
 * @brief 琳达秀入口（MCP 调用，异步执行）
 */
void CuckooStateMachine::StartLindaShow() {
    // MCP 工具入口：仅置标志，实际演出在后台线程执行。
    if (is_running_) return;  // 演出正在进行中，拒绝
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->LindaShow();
        vTaskDelete(nullptr);
    }, "linda_show", 8192, this, 5, nullptr, 1);
}

/**
 * @brief 花园秀入口（MCP 调用，异步执行）
 */
void CuckooStateMachine::StartGardenShow() {
    // MCP 工具入口：仅置标志，实际演出在后台线程执行。
    if (is_running_) return;  // 演出正在进行中，拒绝
    xTaskCreatePinnedToCore([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->GardenShow();
        vTaskDelete(nullptr);
    }, "garden_show", 8192, this, 5, nullptr, 1);
}

/**
 * @brief 琳达秀实现
 * 流程：LED亮→播放0015.mp3→舞蹈电机开场（LED交替闪烁前24秒）→LED全亮→舞蹈至音乐结束→LED熄灭→恢复
 */
void CuckooStateMachine::LindaShow() {
    if (is_running_) { ESP_LOGW(TAG, "LindaShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

    // 防止 AI 说话与背景音乐同时播放（置标志避免冲突）

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "LindaShow: start, playing 0015.mp3");

    // 1. 先开 LED 一起亮起，提示演出开始
    gpio_set_level(LED_A_GPIO, 1);  // LED_A 亮
    gpio_set_level(LED_B_GPIO, 1);  // LED_B 亮
    vTaskDelay(pdMS_TO_TICKS(500));  // 亮 0.5 秒

    // 2. 播放背景音乐（bg audio），避免 TTS 占用 I2S
    if (mp3_) { mp3_->SetDisableDucking(true); int song = LINDA_SONG_BASE + (linda_song_counter_++ % LINDA_SONG_COUNT); ESP_LOGI(TAG, "LindaShow: playing %04d.mp3 (counter=%d)", song, (int)linda_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 8192, this, 4, nullptr); }

    // 等待背景音乐开始排空
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED 交替闪烁 + 舞蹈电机旋转，直到音乐结束
    //    前24秒：LED 交替闪烁 + 舞蹈
    //    24秒后 LED 全亮，舞蹈继续直到音乐结束
    unsigned long music_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    unsigned long m1_timer = music_start_ms;
    int m1_stage = 0;
    int m1_rand_time = 1000 + (esp_random() % 2001);
    int led_toggle = 0;
    int led_state = 0;
    bool led_final = false;
    m1_fwd_time_ = 0;  // 正转累计时间
    m1_rev_time_ = 0;  // 反转累计时间
    m1_stage_start_ = music_start_ms;

    while (is_running_ && Application::GetInstance().GetAudioService().IsBgAudioActive()
           && (xTaskGetTickCount() * portTICK_PERIOD_MS - music_start_ms) < 120000UL) {  // 120s safety timeout
        unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        unsigned long elapsed = now - music_start_ms;

        // 音乐停止前约2秒（24秒时）LED 全亮，停止闪烁
        if (!led_final && elapsed >= 24000) {
            gpio_set_level(LED_A_GPIO, 1);
            gpio_set_level(LED_B_GPIO, 1);
            led_final = true;
        }

        // 舞蹈电机旋转时间 1~3 秒，随机停顿后继续
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
        // LED 交替闪烁（仅前24秒），每6帧切换一次 = 约300ms 闪烁
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

    // 5. 音乐结束 → 舞蹈停 → 播放感谢语音 → LED 熄灭
    if (m1_) m1_->Stop();  // 停舞蹈电机

    // 按净角度补偿（279°/s）
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

    // 播放感谢语音（文件编号0~4循环）
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "LindaShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

    gpio_set_level(LED_A_GPIO, 0);  // LED_A 灭
    gpio_set_level(LED_B_GPIO, 0);  // LED_B 灭

    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // 停 bg audio
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // 停 bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);  // 恢复 duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "LindaShow: done");
}

/**
 * @brief 花园秀实现
 * 流程：LED亮→播放0016.mp3→小提琴舵机0~140度摆动+LED交替闪烁（前24秒）→
 * LED全亮→小提琴至音乐结束→平滑回中位→播放感谢语音→LED熄灭→恢复
 */
void CuckooStateMachine::GardenShow() {
    if (is_running_) { ESP_LOGW(TAG, "GardenShow: already running, skip"); return; }

    auto& app = Application::GetInstance();

    // 防止 AI 说话与背景音乐同时播放（置标志避免冲突）

    is_running_ = true;
    current_performance_ = kPerformanceManual;
    MotorPowerOn();
    ESP_LOGI(TAG, "GardenShow: start, playing 0016.mp3");

    // 1. 先开 LED 一起亮起，提示演出开始
    gpio_set_level(LED_A_GPIO, 1);  // LED_A 亮
    gpio_set_level(LED_B_GPIO, 1);  // LED_B 亮
    vTaskDelay(pdMS_TO_TICKS(500));  // 亮 0.5 秒

    // 2. 播放背景音乐（bg audio），避免 TTS 占用 I2S
    if (mp3_) { mp3_->SetDisableDucking(true); int song = GARDEN_SONG_BASE + (garden_song_counter_++ % GARDEN_SONG_COUNT); ESP_LOGI(TAG, "GardenShow: playing %04d.mp3 (counter=%d)", song, (int)garden_song_counter_); show_music_index_ = song; xTaskCreate([](void* arg) { auto* s = (CuckooStateMachine*)arg; s->PlayShowMusicBg(s->show_music_index_); vTaskDelete(nullptr); }, "show_bg", 8192, this, 4, nullptr); }

    // 等待背景音乐开始排空
    int wait = 0;
    auto& audio = Application::GetInstance().GetAudioService();
    while (wait < 20 && is_running_ && !audio.IsBgAudioActive()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
    }

    // 3. LED 闪烁 + 小提琴舵机摆动，直到音乐结束
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

        // 小提琴摆动：每50ms移动6度，0~140度左右摆动
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

    // 5. 音乐结束 → 小提琴平滑回位 → 播放感谢语音 → LED 熄灭
    if (violin_servo_) {
        violin_servo_->Sweep(violin_angle, SERVO_CENTER_ANGLE, 1000);  // 从当前角度平滑回到90度
    }

    // 播放感谢语音（文件编号0~4循环）
    int thanks_idx = 2001 + (thanks_counter_++ % 5);
    char thanks_file[32];
    snprintf(thanks_file, sizeof(thanks_file), "%d.wav", thanks_idx);
    ESP_LOGI(TAG, "GardenShow: playing thanks: %s (counter=%d)", thanks_file, (int)thanks_counter_);
    PlayWavAsset(thanks_file);

    gpio_set_level(LED_A_GPIO, 0);  // LED_A 灭
    gpio_set_level(LED_B_GPIO, 0);  // LED_B 灭

    if (mp3_) mp3_->Stop();
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // 停 bg audio
    Application::GetInstance().GetAudioService().EnableBgAudioDrain(false);  // 停 bg audio
    MotorPowerOff();
    is_running_ = false;
    current_performance_ = kPerformanceNone;

    app.GetAudioService().SetOutputMuted(false);
    if (mp3_) mp3_->SetDisableDucking(false);  // 恢复 duck
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    ESP_LOGI(TAG, "GardenShow: done");
}

/**
 * @brief 舞蹈表演（Arduino 风格）：M1 正反转交替约 8 秒
 * 正转1~3秒→停20ms→反转1~3秒→停20ms 循环；
 * 同时小提琴电机持续旋转 + 小提琴舵机 45↔135° 摆动；结束后全部停止并复位。
 */
void CuckooStateMachine::Dance() {
    if (is_running_) return;
    MotorPowerOn();
    is_running_ = true;
    current_performance_ = kPerformanceManual;
    ESP_LOGI(TAG, "Dance start (Arduino-style)");

    // M1舞蹈节奏: 正转1-3秒 → 停20ms → 反转1-3秒 → 停20ms → 循环（总约8秒）
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

 // ：（板 B 的 M2 通过 violin_motor_ 控制）
        if (violin_motor_) violin_motor_->Forward(VIOLIN_WARMUP_PERCENT);

        // 小提琴摆动: 45→135 循环摆动
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
// 电机速度测试：舞蹈电机正转指定秒数
// 用法: cuckoo.motor_test (seconds: 1~60)
// ============================================
void CuckooStateMachine::MotorTest(int seconds) {
    if (is_running_) return;
    is_running_ = true;
    MotorPowerOn();
    ESP_LOGI(TAG, "MotorTest: M1 forward %d seconds...", seconds);
    if (m1_) m1_->Forward(100);  // GPIO直接全速电压
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    if (m1_) m1_->Stop();
    MotorPowerOff();
    is_running_ = false;
    ESP_LOGI(TAG, "MotorTest: done");
}


/**
 * @brief 综合表演入口（MCP cuckoo.start_show 调用）
 * 在 Core 1 创建 cuckoo_show 任务 → StartShowTask
 */
void CuckooStateMachine::StartShow() {
    // 演出/报时进行中，拒绝新的 Show 请求（避免与 AI 冲突）
    if (is_running_ && current_performance_ != kPerformanceNone) {
        ESP_LOGW(TAG, "Performance already running (type=%d), ignoring show request",
                 (int)current_performance_);
        return;
    }
    // 若有旧演出（其他类型）先停，稍停后重新开始
    if (is_running_) {
        StopAll();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // 标记占用，防止重复调用
    is_running_ = true;
    current_performance_ = kPerformanceManual;

    // 异步执行：MCP 调用运行在主循环线程上，app.Schedule 排队
    // 若主循环忙，需等待 AbortSpeaking/TTS-STOP 完成才能执行
    // 而 Schedule 回调推不上队列时，TTS-STOP 的状态转换无法执行
    // 所以直接把演出放到 Core 1 的后台任务执行。
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
 * @brief 综合表演后台任务
 * 等 AI 安静后启动：LED+水车+随机选歌+舞蹈+小狗表演(异步触发)+舞蹈循环+LED收尾+恢复
 */
void CuckooStateMachine::StartShowTask() {
    auto& app0 = Application::GetInstance();
    // 立即开始 —— ShowTask 现在用 PlayShowMusicBg（背景音频环形缓冲）
    // 音频服务通过 ducking 与 TTS 混音，无 I2S 冲突。
    ESP_LOGI(TAG, "Show: starting immediately (bg audio + ducking handles overlap)");

    app0.GetAudioService().SetWakeWordThreshold(0.30f);

MotorPowerOn();
    ESP_LOGI(TAG, "Show starting: dance + dog + music");

    ShowTask(this);
}

/**
 * @brief 综合表演执行（ShowTask 入口调用的实际逻辑）
 * 经 PerformanceTask 调用 RunDanceIntro/RunDanceLoop/RunDanceFinale
 */
void CuckooStateMachine::ShowTask(void* arg) {
    auto* sm = static_cast<CuckooStateMachine*>(arg);
    sm->violin_state_.Reset();
    sm->dog_state_.Reset();
    ESP_LOGI(TAG, "Show task started");

    auto& app = Application::GetInstance();

    // 防止 AI 与舞蹈同时播放
    if (sm->mp3_) sm->mp3_->SetDisableDucking(true);  // 演出时音乐不被压低

    // ====== 演出开始：LED 亮 + 水车转 ======
    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);
        if (sm->water_bird_) sm->water_bird_->SetSpeed(WATER_WHEEL_SPEED);  // 水车 IN1=HIGH

    // 随机选歌（避免与上次重复）
    int next = (esp_random() % 12) + 1;
    if (next == sm->show_music_index_ && next < 12) {
        next++;
    } else if (next == sm->show_music_index_) {
        next = 1;
    }
    sm->show_music_index_ = next;
    // 和 LindaShow/GardenShow 一样用 PlayShowMusicBg（背景音频环形缓冲）
    // 音频服务通过 ducking 与 TTS 混音 —— 无 I2S 冲突。
    if (sm->mp3_) {
        sm->mp3_->SetDisableDucking(true);
        xTaskCreatePinnedToCore([](void* arg) {
            auto* s = (CuckooStateMachine*)arg;
            s->PlayShowMusicBg(s->show_music_index_);
            vTaskDelete(nullptr);
        }, "show_bg", 8192, sm, 4, nullptr, 1);
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

    // 异步触发小狗+小提琴出场（不阻塞舞蹈循环）
    xTaskCreatePinnedToCore([](void* arg) {
        auto* s = static_cast<CuckooStateMachine*>(arg);
        s->RunDanceIntro();
        vTaskDelete(nullptr);
    }, "dance_intro", 2048, sm, 5, nullptr, 1);

    // 舞蹈+小提琴同步开始，等待舞蹈/小提琴演出结束
    sm->RunDanceLoop();

    // 音乐结束后恢复唤醒词和音频输入
    // 注意：StartShow 中 AbortSpeaking 将状态改为 listening，而非 idle
    // 直接恢复唤醒词会导致唤醒词在 listening 状态，AbortSpeaking 修改 audio_input 缓冲


    if (sm->mp3_) sm->mp3_->SetDisableDucking(false);  // 恢复 duck

    // 确保设备回到 idle 再恢复唤醒词检测
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Show: device not idle (state=%d), aborting to return to idle", (int)app.GetDeviceState());
        app.AbortSpeaking(kAbortReasonNone);
        for (int i = 0; i < 10 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "Show: music ended, running dance finale");

    sm->RunDanceFinale();

    // 在收尾完成后才清理背景音频（之前是在前面清理，导致状态间隙）
    app.GetAudioService().EnableBgAudioDrain(false);
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "Show: bg audio cleaned up, wake word threshold will be restored by clock task");
    sm->is_running_ = false;
    sm->current_performance_ = kPerformanceNone;

    // ====== 停水车 + 关 LED ======
    if (sm->water_bird_) sm->water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);

    sm->MotorPowerOff();

    // 恢复唤醒词阈值（设备处于 idle 时，时钟任务不再触发状态转换）
    // 修复(2026-07-19)：演出期间设备保持 idle，无状态转换，
    // 时钟任务从不恢复 0.02 → 卡在 0.30（难以唤醒）。
    if (app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    else
        app.GetAudioService().SetWakeWordThreshold(0.30f);

    ESP_LOGI(TAG, "Show task finished");
    vTaskDelete(NULL);
}

/**
 * @brief 播放指定编号音乐（封装 Mp3Player::PlayBgMusic）
 * @param index MP3 编号
 */
void CuckooStateMachine::PlayMusic(int index) {
    if (mp3_) mp3_->PlayBgMusic(index);
}

/**
 * @brief 立即停止所有演出和电机
 * - 停止所有电机 M1~M4 + 小提琴 + 水车
 * - 小提琴回到 90 度
 * - LED 熄灭
 * - 保持音乐播放不停止（除非是表演模式）
 * - 通过 Schedule 异步等待设备状态为 Idle 后恢复唤醒词
 */
void CuckooStateMachine::StopAll() {
    is_running_ = false;
    current_performance_ = kPerformanceNone;
    current_phase_ = kPhaseIdle;
    if (m1_) m1_->Stop();  // 舞蹈
    if (m2_) m2_->Stop();  // 大门
    if (m3_) m3_->Stop();  // 小狗
    if (m4_) m4_->Stop();  // 电机4
    if (violin_motor_) violin_motor_->Stop();  // 小提琴电机
    if (violin_servo_) violin_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (dog_servo_) dog_servo_->SetAngle(SERVO_CENTER_ANGLE);
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    // 注意 stop_all+play_url 会重复播放背景音乐
    if (mp3_) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (audio.IsBgAudioActive()) {
            ESP_LOGI(TAG, "StopAll: music playing, not stopping background audio");
        } else {
            mp3_->Stop();
        }
    }

    // 不直接 AbortSpeaking TTS，等其自然结束，避免打断语音
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
 * @brief 停止音乐播放，恢复音频输入
 */
void CuckooStateMachine::StopMusic() {
    kids_dance_ = false;
    if (mp3_) {
        mp3_->Stop();
        Application::GetInstance().GetAudioService().ClearBackgroundAudio();
    }
}

/**
 * @brief 大门打开（M2 正转固定时长 MAIN_DOOR_TIME_MS）
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
 * @brief 大门关闭（M2 反转固定时长 MAIN_DOOR_TIME_MS）
 */
void CuckooStateMachine::CloseDoor() {
    if (!door_open_.load()) {
        ESP_LOGI(TAG, "Door: already closed, skip motor");
        return;
    }
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
 * @brief 小鸟跳跃一次（通电约500ms + 上电冷却400ms）
 */
void CuckooStateMachine::BirdJumpOnce() {
    MotorPowerOn();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    MotorPowerOff();
}

/**
 * @brief 小鸟跳跃脉冲：通电 500ms 后断电（单次，无冷却等待）
 */
void CuckooStateMachine::BirdJumpPulse() {
    MotorPowerOn();
    if (water_bird_) water_bird_->Stop();
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);
}

/**
 * @brief 小鸟短促跳跃（AI 说话时模拟插话的鸟鸣）
 * 随机 50-200ms 通电脉冲，冷却 300-700ms 后循环
 */
void CuckooStateMachine::BirdJumpShort() {
    // AI 说话时小鸟模拟插话跳跃（随机节奏模式）
    // 通电时间随机 50-200ms 模拟自然跳跃
    // 冷却时间随机 300-700ms 防止跳跃太频繁
    MotorPowerOn();
    if (water_bird_) water_bird_->Stop();  // 先停水车释放 GPIO39 避免冲突
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);  // 电机通电，小鸟弹出
    int pulse = 50 + (esp_random() % 151);  // 随机 50-200ms 通电脉冲
    vTaskDelay(pdMS_TO_TICKS(pulse));
    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);  // 电机断电，小鸟收回
    int cooldown = 300 + (esp_random() % 401);  // 随机 300-700ms 冷却时间
    vTaskDelay(pdMS_TO_TICKS(cooldown));
}

