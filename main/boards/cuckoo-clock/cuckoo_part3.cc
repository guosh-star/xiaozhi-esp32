// ===== Part 3: LdrSensor、BellSoundPlayer、Dance (L1925-2394) =====
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
    return raw;
        // raw: 0~4095 (12-bit ADC)，数值越小=越暗
}

/**


 */
/**
bool LdrSensor::IsDark() { return ReadRaw() < threshold_; }
 * @brief 判断当前是否为黑暗（光线低于阈值）
void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }
 * @return true=黑暗, false=明亮

 */

// ============================================
// BellSoundPlayer - 布谷鸟语音 + 报时钟声播放
// BellSoundPlayer - 布谷鸟语音 + 报时钟声播放
// ============================================
// 同步播放布谷鸟唤醒音（阻塞，直接写入音频输出）
void BellSoundPlayer::PlayCuckooSoundSync() {

    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_wake_sound,
        CUCKOO_WAKE_SOUND_NUM_SAMPLES,
        CUCKOO_WAKE_SOUND_SAMPLE_RATE
    );
}

// 同步播放钟铃声（阻塞，使用 data_if_mutex_ 与 AudioOutputTask 互斥）
void BellSoundPlayer::PlayBellSoundSync() {

    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_bell_sound,
        CUCKOO_BELL_SOUND_NUM_SAMPLES,
        CUCKOO_BELL_SOUND_SAMPLE_RATE
    );
}

// 异步播放：在新任务中执行，不阻塞调用者
void BellSoundPlayer::PlayCuckooSoundAsync() {
    xTaskCreate([](void* arg) {
        auto* self = static_cast<BellSoundPlayer*>(arg);
        self->PlayCuckooSoundSync();
        vTaskDelete(NULL);
    }, "cuckoo_async", 2048, this, 5, NULL);
}

// ============================================

// ============================================
CuckooStateMachine::CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                                        Motor* violin_motor, Servo* violin, Servo* dog,
                                        Motor* water_bird, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
      violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),
            // 小提琴舵机（IN B M2, GPIO18/45）
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

        // 电机电源 P-MOSFET 控制 (GPIO LOW=ON 导通, HIGH=OFF 断开)
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);
     // LED 指示灯 (S8050 NPN 驱动, 5V 供电)
 // LED (GPIO1KS8050 B, C, 5V)
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_A_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_cfg);
        // LED 指示灯 (S8050 NPN 驱动, 5V 供电)
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    MotorPowerOff();
  }

CuckooStateMachine::~CuckooStateMachine() {
    StopAll();
    MotorPowerOff();
}

/**

 */
/**
void CuckooStateMachine::MotorPowerOn() {
 * @brief 打开电机电源（P-MOSFET 导通→5V 供电）
    gpio_set_level(motor_power_pin_, 0);
 */
}

void CuckooStateMachine::MotorPowerOff() {
    gpio_set_level(motor_power_pin_, 1);
}

// ============================================

// ============================================


// 异步开门任务上下文
struct DoorOpenCtx {
    CuckooStateMachine* sm;
};

// 异步开门任务：上电 → 正向开门 → 延时 → 停止
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

        // 关键路径：如果背景音频正在播放，叠加混音（不打断音乐）；否则用 OutputRawPcm 直出（如 DogShow）
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

// 舞蹈开场：异步开门同时放出小狗（舵机扫出 + 前进 + 叫一声）
void CuckooStateMachine::RunDanceIntro() {

    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    xTaskCreatePinnedToCore(DoorOpenTask, "door_open", 2048, ctx, 5, nullptr, 1);


    if (dog_servo_) {
            // 小狗尾巴：180°→20° 扫出，准备前进
        dog_servo_->Sweep(180, 20, (180 - 20) * 15);
    }

    if (m3_) {
        m3_->Forward(DOG_SPEED_PERCENT);
        vTaskDelay(pdMS_TO_TICKS(DOG_WALK_TIME_MS));
        m3_->Stop();
    }
    PlayDogBark();

    if (dog_servo_) {
            // 小狗进门后摇尾归零
        dog_servo_->Sweep(20, 0, 20 * 15);
    }
}

// 舞蹈循环：M1 正反转交替 + 小提琴手臂摆动 + 小狗摇尾 + LED 闪烁 + 水车旋转
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

        // 等待背景音乐启动（最多 5 秒），避免循环内立刻退出
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
        if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);
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


                // 小提琴 10 段循环动作：左右摆动 + 正转/反转拉琴，随机组合
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
                    // 小狗摇尾：随机方向和频率摆动，40~60° 范围
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
                // LED 交替闪烁：每 6 帧（~300ms）切换一次
        if (led_toggle >= 6) {
            led_toggle = 0;
            led_state = !led_state;
            gpio_set_level(LED_A_GPIO, led_state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, led_state ? 0 : 1);
        }


                // 精确 50ms 帧率控制（20fps），用微秒级定时避免累积漂移
        next_frame_us += 50000;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 500) / 1000));
        }
    }
}

// 舞蹈收尾：M1 角度归零、小提琴平衡、小狗回退关门
void CuckooStateMachine::RunDanceFinale() {

    gpio_set_level(LED_A_GPIO, 1);
    gpio_set_level(LED_B_GPIO, 1);


    if (m1_fwd_time_ > 0 || m1_rev_time_ > 0) {
            // ---- M1 齿轮角度归零：根据正反转时间差（转速 ~279°/s）计算需要补偿的角度 ----
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


        // ---- 小提琴手臂平衡：统计正反转次数差，补转使手臂大致归中 ----
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
            // 小狗尾巴归位：从当前角度扫描回 180°（关门）
        int dog_cur = dog_state_.angle;
        dog_servo_->Sweep(dog_cur, 180, (180 - dog_cur) * 15);
        dog_state_.angle = 180;
    }

    MotorPowerOn();
    if (m2_) {
            // 大门关闭
        m2_->Reverse(MAIN_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
        m2_->Stop();
    }
}

// ============================================

// ============================================

// NVS 持久化存储：闹钟数据
static const char* kAlarmNvsNamespace = "cuckoo_alarm";
static const char* kAlarmNvsKey = "alarms";

// 将闹钟数组写入 NVS 闪存（携带 mutex 锁防并发）
void CuckooStateMachine::SaveAlarmsToNvs() {
    nvs_handle_t handle;
    // NVS 持久化存储：闹钟数据
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
        // NVS 持久化存储：闹钟数据
        ESP_LOGI(TAG, "Alarms saved to NVS (%d slots)", kMaxAlarms);
    }
}

// 从 NVS 闪存读取闹钟数组，统计有效闹钟数
void CuckooStateMachine::LoadAlarmsFromNvs() {
    nvs_handle_t handle;
    // NVS 持久化存储：闹钟数据
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
    // NVS 持久化存储：闹钟数据
    ESP_LOGI(TAG, "Alarms loaded from NVS (%d active)", count);
}

