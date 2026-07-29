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
        // MusicDanceTick log muted to reduce noise (prints every 270ms)
        if (music_dance_enabled_ != 1) {
            music_dance_phase_ = 0;
            music_dance_enabled_ = 1;
            dog_outro_done_ = false;
            was_kids_active = kids_now;
        }

        // Dog comes out when music plays &amp; kids active. NOT gated on
        // music_dance_enabled_ init block which can be skipped by stale state.
        if (kids_now && !dog_intro_running_ && !dog_intro_done_) {
            dog_intro_running_ = true;
            MusicDogIntro();
        }

        // Mid-music transition: kids became active → bring dog out
        if (!was_kids_active && kids_now && !dog_intro_running_ && !dog_intro_done_) {
            was_kids_active = true;
            dog_outro_done_ = false;
            dog_intro_running_ = true;
            MusicDogIntro();
        }
        // Mid-music transition: kids became resting → stop & put dog away
        if (was_kids_active && !kids_now) {
            was_kids_active = false;
            if (m1_) m1_->Stop();
            if (violin_servo_) violin_servo_->SetAngle(90);
            if (!dog_outro_done_) {
                dog_outro_done_ = true;
                auto ret = xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 6, nullptr, 1);
                if (ret != pdPASS) ESP_LOGE(TAG, "MusicDanceTick: dog_outro create FAILED ret=%d", (int)ret);
            }
        }

        int phase = music_dance_phase_ % 8;

        // Wait for dog_intro to finish before taking over servo/motor
        if (kids_now && dog_intro_done_) {
            if (kids_appreciating_ && m1_) {
                // Phase 0-3 fwd 20ms, Phase 4-5 rev 20ms, Phase 6 rev 22ms, Phase 7 rev 24ms
                if (phase <= 3) {
                    m1_->Forward();
                    vTaskDelay(pdMS_TO_TICKS(20));
                    m1_->Stop();
                    m1_music_fwd_count_++;
                } else if (phase == 7) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(24));
                    m1_->Stop();
                    m1_music_rev_count_++;
                } else if (phase == 6) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(22));
                    m1_->Stop();
                    m1_music_rev_count_++;
                } else if (phase >= 4) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(20));
                    m1_->Stop();
                    m1_music_rev_count_++;
                }
            }
            
            // Guitar + Dog servos: sync to same phase rhythm
            // Phase 0-3: forward beat, Phase 4-7: backward beat
            static int guitar_angle = 90;
            static int dog_angle = 40;
            bool forward_beat = (phase <= 3);
            int guitar_target = forward_beat ? 110 : 70;   // +/-20
            int dog_target = forward_beat ? 40 : 10;         // dog wags 10-40

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
        // Don't shut down during an active performance (ShowTask/KidsDanceShow)
        if (is_running_) return;
        // MusicDT-ELSE log muted to reduce noise (prints every 250ms)
        was_kids_active = kids_now;
        if (music_dance_enabled_ == 1) {
            // Balance M1 motor: reverse must match forward
            if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
                int diff = m1_music_fwd_count_ - m1_music_rev_count_;
                ESP_LOGI(TAG, "MusicDance: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
                for (int i = 0; i < diff; i++) {
                    m1_->Reverse();
                    vTaskDelay(pdMS_TO_TICKS(30));
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
                auto ret = xTaskCreatePinnedToCore([](void* arg) {
                    ((CuckooStateMachine*)arg)->MusicDogOutro();
                    vTaskDelete(nullptr);
                }, "dog_outro", 4096, this, 6, nullptr, 1);
                if (ret != pdPASS) ESP_LOGE(TAG, "MusicDanceTick: dog_outro(end) create FAILED ret=%d", (int)ret);
            }
        }
        dog_intro_done_ = false;
        music_dance_enabled_ = 0;
    }
}

void CuckooStateMachine::MusicDogIntro() {
    ESP_LOGI(TAG, "MusicDogIntro: ENTER");
    dog_intro_running_ = true;
    MotorPowerOn();
    auto* ctx = new DoorOpenCtx{this};
    auto ret = xTaskCreatePinnedToCore(DoorOpenTask, "door_open_m", 2048, ctx, 6, nullptr, 1);
    if (ret != pdPASS) ESP_LOGE(TAG, "MusicDogIntro: door_open_m create FAILED ret=%d", (int)ret);
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
    // Re-enable motor power in case MusicDogOutro turned it off mid-way
    MotorPowerOn();
    dog_intro_done_ = true;
    dog_intro_running_ = false;
    dog_out_ = true;
    ESP_LOGI(TAG, "MusicDogIntro: done, handing over to MusicDanceTick");
}

void CuckooStateMachine::MusicDogOutro() {
    dog_outro_running_ = true;
    dog_out_ = false;
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
    dog_outro_running_ = false;
}


/**
 * @brief 音乐中触发完整演出：大门+小狗+全体跳舞
 *
 * 大门关着→开门+小狗出来；大门已开→跳过。
 * 不播放独立音乐（音乐已在播），只触发马达和水车。
 * 仅通过 MCP cuckoo.kids_dance 触发（用户主动招呼才执行）。
 */
void CuckooStateMachine::KidsDanceShow() {
    if (is_running_) { ESP_LOGW(TAG, "KidsDanceShow: already running"); return; }
    is_running_ = true;
    // Set kids_active_ NOW so MusicDanceTick doesn't enter else branch
    // and trigger MusicDogOutro during the door-open delay below
    kids_active_ = true;
    kids_appreciating_ = true;  // M1 light swinging with MusicDanceTick
    music_dance_enabled_ = 1;

    xTaskCreate([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        sm->MotorPowerOn();

        // === 大门+小狗：只有大门关着才执行 ===
        if (!sm->door_open_) {
            ESP_LOGI(TAG, "KidsDance: opening main door + dog out");
            if (sm->m2_) {
                sm->m2_->Forward(MAIN_DOOR_OPEN_SPEED);
                vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));
                sm->m2_->Stop();
            }
            sm->door_open_ = true;
            vTaskDelay(pdMS_TO_TICKS(300));
            // 小狗出来
            if (!sm->dog_out_ && sm->dog_servo_) {
                sm->dog_servo_->Sweep(180, 20, (180 - 10) * 15);
                if (sm->m3_) {
                    sm->m3_->Forward(DOG_SPEED_PERCENT);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    sm->m3_->Stop();
                }
                sm->dog_servo_->Sweep(20, 35, 25 * 15);
                sm->dog_intro_done_ = true;
                sm->dog_out_ = true;
            }
        } else {
            ESP_LOGI(TAG, "KidsDance: main door already open, skipping");
        }

        // === 全体跳舞 ===
        sm->KidsComeOut();
        sm->dog_intro_done_ = true;  // safety: ensure MusicDanceTick IF drives motors
        sm->kids_dance_ = true;
        // Visible dance: same big moves as hourly chime
        sm->RunDanceLoop();
        // MusicDanceTick resume guard: stop M1 + reset state to prevent mid-song race
        if (sm->m1_) sm->m1_->Stop();
        sm->music_dance_enabled_ = 0;
        sm->m1_music_fwd_count_ = 0;
        sm->m1_music_rev_count_ = 0;
        if (sm->kids_dance_) {
            bool music_stopped = !Application::GetInstance().GetAudioService().IsBgAudioActive()
                                 && (!sm->mp3_ || !sm->mp3_->IsPlaying());
            if (music_stopped) {
                // Music ended: full cleanup, all off
                sm->kids_appreciating_ = false;
                gpio_set_level(LED_A_GPIO, 0);
                gpio_set_level(LED_B_GPIO, 0);
                if (sm->water_bird_) sm->water_bird_->Stop();
                sm->MusicDogOutro();
                sm->kids_active_ = false;
            } else {
                // Music still playing: smooth transition to light swinging
                sm->kids_appreciating_ = true;
                gpio_set_level(LED_A_GPIO, 0);
                gpio_set_level(LED_B_GPIO, 0);
            }
            sm->kids_dance_ = false;
        }
        // else: user stopped early, keep door open -> MusicDanceTick takes over
        sm->is_running_ = false;
        ESP_LOGI(TAG, "KidsDanceShow: dance finished, cleaned up");
        vTaskDelete(NULL);
    }, "kids_dance", 4096, this, 4, NULL);
}

void CuckooStateMachine::KidsComeOut() {
    kids_active_ = true;
    dog_out_ = true;
    kids_appreciating_ = true;  // light M1 swinging via MusicDanceTick
    SaveKidsActive();
    ESP_LOGI(TAG, "KidsComeOut: kids active, will dance with music");
    // Safety: if music is playing and dog isn't out yet, force-create dog_intro.
    // Bypasses MusicDanceTick state machine which can miss the dog due to
    // prio-6 task scheduling races between dog_outro and dog_intro on Core 1.
    // User explicitly asked for kids — force create dog_intro regardless of stale state flags.
    // Don't check dog_intro_done_ or dog_intro_running_ which can be corrupted by
    // race conditions with MusicDanceTick and PerformanceTask.
    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();
    if (music_playing && !IsRunning()) {
        dog_outro_done_ = false;
        music_dance_enabled_ = 1;
                music_dance_phase_ = 0;
        dog_intro_done_ = false;
        dog_intro_running_ = true;
        MusicDogIntro();
    }
}

void CuckooStateMachine::KidsRest() {
    if (kids_dance_) { ESP_LOGI(TAG, "KidsRest: stopping dance, sending kids back"); is_running_ = false; kids_dance_ = false; /* force RunDanceLoop exit, then fall through to close */ }
    kids_active_ = false;
    dog_out_ = false;
    kids_appreciating_ = false;  // stop M1 light swinging
    SaveKidsActive();
    // Balance M1 motor: reverse must match forward before stopping
    if (m1_ && m1_music_rev_count_ < m1_music_fwd_count_) {
        int diff = m1_music_fwd_count_ - m1_music_rev_count_;
        ESP_LOGI(TAG, "KidsRest: fwd=%d rev=%d, adding %d reverse pulses", m1_music_fwd_count_, m1_music_rev_count_, diff);
        for (int i = 0; i < diff; i++) {
            m1_->Reverse();
            vTaskDelay(pdMS_TO_TICKS(30));
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
    // Send dog back and close door immediately
    MusicDogOutro();
    dog_outro_done_ = true;  // prevent duplicate cleanup when music ends
    ESP_LOGI(TAG, "KidsRest: kids resting, dog back, door closing");
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

void CuckooStateMachine::StartFlashLeds() {
    if (flash_leds_active_) return;
    flash_leds_active_ = true;
    xTaskCreate([](void* arg) {
        auto* sm = static_cast<CuckooStateMachine*>(arg);
        int state = 0;
        while (sm->flash_leds_active_) {
            state = !state;
            gpio_set_level(LED_A_GPIO, state ? 1 : 0);
            gpio_set_level(LED_B_GPIO, state ? 0 : 1);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        vTaskDelete(NULL);
    }, "flash_leds", 2048, this, 4, NULL);
}

void CuckooStateMachine::StopFlashLeds() {
    flash_leds_active_ = false;
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

    // URL编码
    char encoded_path[512];
    // 第0步：统一路径格式，把 /opus 或 /stream 都转成 /pcm
    char conv_url[512];
    strncpy(conv_url, url_or_path, sizeof(conv_url) - 1);
    conv_url[sizeof(conv_url) - 1] = '\0';
    ConvertToPcmUrl(conv_url, sizeof(conv_url));
    
    int ep = 0;
    for (const char* p = conv_url; *p && ep < (int)sizeof(encoded_path) - 4; p++) {
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

    // 第2步：构造完整URL，直接用原始路径不转换
    // /pcm 本身就能返回JSON或PCM，无需改路径
    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
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
    auto& mcp = McpServer::GetInstance();

 // === Time / Chime ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.performance",
            "Hourly chime: bell rings + music. ONLY call when user explicitly says ��ʱ/���㱨ʱ/����/what time. DO NOT auto-call on wake-up. For shows/singing/dancing use cuckoo.start_show instead.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int hour = props["hour"].value<int>();
                state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
                return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
            });
    }
    // === LED Flash (2026-07-29) ===
    mcp.AddTool("cuckoo.flash_leds",
        "让两串LED灯串交替闪烁（300ms间隔）。触发词: 闪灯/灯闪起来/灯光闪烁。"
        "关灯用 cuckoo.set_led state=0。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartFlashLeds();
            return std::string("{\"status\": \"leds_flashing\"}");
        });

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
        "�ۺϱ��ݣ��赸+С��+ˮ��+����һ������ '����' '��Ŀ' '��������' '����' '�ݳ�' ���ۺϱ�������"
        "ע�⣺����û�ֻ�ᵽĳ����ɫ��԰��/�մ�/С��������Ҫ�ô˹��ߣ����ö�Ӧ�Ľ�ɫ���ߡ�ʶ���ı������ӽ���ɫ��ʱ����'Ӧ��''�յ�''�ִ�'���մ'ԭַ'��԰�ӣ���Ҳ�ö�Ӧ��ɫ���ߣ���Ҫ�ô˹��ߡ�",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartShow();
            return std::string("{\"status\": \"show_started\"}");
        });

    mcp.AddTool("cuckoo.stop_all",
        "Stop motors, chime, performance. Does NOT stop music - use cuckoo.stop_music to stop music.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAll();
            return std::string("{\"status\": \"stopped\"}");
        });

    mcp.AddTool("cuckoo.stop_music",
        "Stop music playback. Call ONLY when user explicitly asks to stop the music (ͣ��/��ͣ����/��Ҫ����/�ص�). Do NOT call this for performance or alarm - use cuckoo.stop_all for those.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopMusic();
            return std::string("{\"status\": \"music_stopped\"}");
        });

    // === 小朋友们控制 ===
    mcp.AddTool("cuckoo.kids_come_out",
        "让小朋友们出来一起欣赏音乐：小狗舵机、舞蹈电机、吉他舵机随音乐节奏摆动。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsComeOut();
            return std::string("{\"status\": \"kids_come_out\"}");
        });

    mcp.AddTool("cuckoo.kids_rest",
        "让小朋友们回去休息：停止所有动作（小狗舵机、舞蹈电机、吉他舵机不动），只保留音乐播放。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsRest();
            return std::string("{\"status\": \"kids_rest\"}");
        });


    mcp.AddTool("cuckoo.kids_dance",
        "让小朋友们一起跳舞：开门+小狗出来+全体跳舞。触发词: 让小朋友们跳舞/一起跳舞/跳舞吧。"
        "【只在正在播放音乐时使用】如果当时没放音乐，绝不用此工具——改用cuckoo.start_show。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait.\"}");
            state_machine_->KidsDanceShow();
            return std::string("{\"status\": \"kids_dance_started\"}");
        });

 // === Hardware (wiring later) ===
    mcp.AddTool("cuckoo.dance",
        "Dance routine: M1+M2 motors + violin servo.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->Dance();
            return std::string("{\"status\": \"dancing\"}");
        });


    {
        PropertyList pl;
        pl.AddProperty(Property("state", kPropertyTypeInteger, 0, 1));
        mcp.AddTool("cuckoo.set_door",
            "Open (state=1) or close (state=0) the main door.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int state = props["state"].value<int>();
                if (state != 0) state_machine_->OpenDoor();
                else state_machine_->CloseDoor();
                char buf[32];
                snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"door\":%d}", state  ?  1 : 0);
                return std::string(buf);
            });
    }

    {
        PropertyList pl;
        pl.AddProperty(Property("state", kPropertyTypeInteger, 0, 1));
        mcp.AddTool("cuckoo.set_led",
            "Turn both LED strings on (state=1) or off (state=0).",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int state = props["state"].value<int>();
                int v = (state != 0) ? 1 : 0;
                state_machine_->StopFlashLeds();  // stop flashing
                gpio_set_level(LED_A_GPIO, v);
                gpio_set_level(LED_B_GPIO, v);
                char buf[32];
                snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"led\":%d}", v);
                return std::string(buf);
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
        "Stop a ringing ALARM only. For ������/ͣ����. NOT for stopping music/performance - use cuckoo.stop_all for that.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === Quiet Mode ===
    {
        PropertyList pl;
        pl.AddProperty(Property("mode", kPropertyTypeInteger, 0, 3));
        pl.AddProperty(Property("start_hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("end_hour", kPropertyTypeInteger, 0, 23));
        mcp.AddTool("cuckoo.set_quiet_mode",
            "Set chime quiet mode. 0=ȫ�쾲��(������ʱ), 1=ȫ�챨ʱ, 2=��ھ���(LDR����), 3=ָ��ʱ��ξ���(start_hour~end_hour����). "
            "Mode 2 uses light sensor only (no time limit). Mode 3 defaults to 22-6.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int m = props["mode"].value<int>();
                int sh = props["start_hour"].value<int>();
                int eh = props["end_hour"].value<int>();
                state_machine_->quiet_mode_ = m;
                state_machine_->quiet_start_ = sh;
                state_machine_->quiet_end_ = eh;
                state_machine_->SaveQuietMode();
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"status\": \"ok\", \"mode\": %d, \"start_hour\": %d, \"end_hour\": %d}", m, sh, eh);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_quiet_mode",
        "Get quiet mode (0=silent-24h, 1=chime-24h, 2=dark-silent-LDR-only, 3=time-window-22-6). "
        "Mode 2: no time limit, only LDR brightness matters. Mode 3: uses start_hour/end_hour only.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            int m = state_machine_->quiet_mode_.load();
            int sh = state_machine_->quiet_start_.load();
            int eh = state_machine_->quiet_end_.load();
            char json[192];
            snprintf(json, sizeof(json),
                "{\"mode\": %d, \"start_hour\": %d, \"end_hour\": %d, "
                "\"desc\": \"mode=%d: %s\"}",
                m, sh, eh, m,
                m == 0 ? "ȫ�쾲��" : m == 1 ? "ȫ�챨ʱ" :
                m == 2 ? "��ھ���(LDR)" : "ʱ��ξ���");
            return std::string(json);
        });

    // === 状态查询 ===
    mcp.AddTool("cuckoo.get_status",
        "查询设备状态：running（是否正在表演），music_playing（音乐是否正在播放）。\n"
        "【强制规则】回复用户任何关于演出/表演/音乐/歌舞的问题前，必须先调用本工具。不得凭记忆或上下文猜测状态。\n"
        "如果 running 是 false 但用户提到演出，直接说演出已经结束，不要说还在进行。",
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

 // === 在线音乐 ===
 // 音乐代理在启动时自动配置（config.h 里的 DEFAULT_MUSIC_PROXY_HOST/PORT）。
 // set_music_proxy 是空操作占位工具，永远瞬间成功，不做TCP连接检查。
 // 这个工具只是为了满足 AI 在调用 play_url 之前先调 set_music_proxy 的习惯。

    {
        PropertyList pl;
        pl.AddProperty(Property("url", kPropertyTypeString));
        mcp.AddTool("cuckoo.play_url",
            "播放在线音乐。用户说出歌名后立即调用本工具，不要先问用户问题。\n""参数url格式：'/pcm?q=歌名'（中文需要URL编码，空格用%%20）。\n""【歌手规则】如果用户明确指定了歌手（如'放苏芮的奉献'），url格式为'/pcm?q=歌名 歌手'，系统直接播该歌手版本。如果用户只说歌名没指定歌手（如'放《奉献》'），url只用'/pcm?q=歌名'，让系统自动展示多版本供选择。\n""如果正在放歌，用户要换歌，直接传新歌名即可，系统自动停旧播新。\n""不要问用户'要不要换'——直接调用工具。\n""不要光说'我来放歌'——必须实际调用play_url。\n""示例：'放酒干倘卖无'→play_url('/pcm?q=%E9%85%92%E5%B9%B2%E5%80%98%E5%8D%96%E6%97%A0')。'放苏芮的奉献'→play_url('/pcm?q=%E5%A5%89%E7%8C%AE%%20%E8%8B%8F%E8%8A%AE')。\n""本地歌曲请用cuckoo.play_music（曲目1-12），不要用play_url。",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string url = props["url"].value<std::string>();
                // 先检查多版本：如果服务器返回歌手列表JSON，直接返回给AI念（不播歌）
                std::string multi = state_machine_->CheckMultiArtist(url.c_str());
                if (!multi.empty()) {
                return multi;  // 歌手列表JSON返回给AI，AI自动念出来让用户选
                }
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
    // === ��ɫ���� ===
    mcp.AddTool("cuckoo.dog_show",
        "小狗丽莎表演：开门、小狗跑出来、叫一声、摇头摆尾10秒、再叫一声、退回、关门。说完只回一句简短的话，不要多说。用户说 丽莎在哪里 / 丽莎，丽莎 / 丽莎出来 时调用此工具。Dog show: call when user asks about dog/puppy/Lisa. Keep response very brief.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->DogShow();
            return std::string("{\"status\": \"dog_show_started\"}");
        });

    mcp.AddTool("cuckoo.linda_show",
        "琳达表演：播放0015号音乐、舞蹈电机转动、直到音乐结束。用户说 琳达，琳达 / 琳达在哪里？ / 琳达出来跳个舞 时调用此工具。Linda show: call when user asks about Linda or dancing.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartLindaShow();
            return std::string("{\"status\": \"linda_show_started\"}");
        });

    mcp.AddTool("cuckoo.garden_show",
        "园子表演：播放0016号音乐、小提琴舵机转动、直到音乐结束。用户说 园子，园子 / 园子在哪里？ / 园子弹个吉他 时调用此工具。Garden show: call when user asks about Garden/violin.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartGardenShow();
            return std::string("{\"status\": \"garden_show_started\"}");
        });

    // === Hourly Performance Toggle ===
    {
        PropertyList pl;
        pl.AddProperty(Property("enabled", kPropertyTypeBoolean, true));
        mcp.AddTool("cuckoo.set_hourly_performance",
            "Enable/disable LED, water wheel, music and dance after hourly bell. Default ON. Important: turning this OFF does NOT stop the bird calls or bell chime. The bird still comes out and calls every hour. Only the extras (LED, water wheel, music, dance) are skipped.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                bool en = props["enabled"].value<bool>();
                state_machine_->hourly_perf_.store(en);
                state_machine_->SaveHourlyPerf();
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"performance\":%s}", en ? "enabled" : "disabled");
                return std::string(buf);
            });
    }
    
    mcp.AddTool("cuckoo.get_hourly_performance",
        "Check if hourly chime performance is currently enabled.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            bool en = state_machine_->hourly_perf_.load();
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"performance\":%s}", en ? "enabled" : "disabled");
            return std::string(buf);
        });

}

// ============================================
// �ӿ� FreeRTOS ���� (Core 1)
//
// ����: 250ms sub-tick, 1s per tick
// ְ��:
// - NTPʱ��ͬ����60s/300s����У׼��
// - �豸״̬��أ�idle?�����Զ��������ţ�AI˵��ʱС�������Ծ��
// - ���Ѵ���ֵ��̬�������Ի���0.3����ֹ�󴥷���idleʱ0.02��������
// - ��Ƶʱ���ˢ�£�ÿ10s��ֹI2S��Դ�����رգ�
// - ����/��㱨ʱ����
// - ���Ӽ��
// - RTC������־��ÿ30s���浽�������ڴ棩
// ============================================
void cuckoo_clock_task(void* params) {
    // ������ȡ����ԭ�� + RTC ����
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

    // ����ʱ����ˢ����Ƶʱ�������ֹ�����ڼ�I2S���Ź���ʱ
    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();

    // ������ WiFi ������ NTP gettimeofday ���ɻ�ȡ��ǰʱ��

    // ������ WiFi ������ NTP gettimeofday ���ɻ�ȡ��ǰʱ��
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1000000000) {  // 2001���Ժ�˵�� NTP ��ͬ��
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

    // ����Ĭ�����ִ�����ַ������ config.h ����ʱ�̶���
    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);

    // �� NVS �����ѱ��������;���ģʽ
    sm->LoadAlarmsFromNvs();
    sm->LoadQuietMode();
    sm->LoadKidsActive();

uint32_t tick_sec = 0;

 // TODO(#22): ��1����ѯ��Ϊ�¼�����:
 // - ע�� OnDeviceStateChanged �ص����� idle/active ת��
 // - ��������ʱ����NTPͬ������tick_sec����
 // - ����Core 1�󲿷�ʱ���������ʡ��
    uint32_t sub_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        sub_tick++;

        // ����豸��idle�����û�����AI����¼ʱ������ڷ��󴥷�
        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, "Device woke up �� opening bird door");
                sm->OpenBirdDoor();

                // �Ի�����߻��Ѵ���ֵ��0.3������ֹ�󴥷��������ڼ䲻�裬��Show�Լ����ƣ�
                if (!sm->IsRunning())
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.3f);

                // �Ի��лָ���׼��˷����棨30dB�������� AEC ���������Ŵ�
                Application::GetInstance().GetAudioService().SetInputGain(30.0f);
            } else if (sm->prev_device_state_ != (int)kDeviceStateIdle && dev_state == (int)kDeviceStateIdle) {
                ESP_LOGI(TAG, "Device sleeping �� closing bird door");
                sm->CloseBirdDoor();

                // idleʱ�ָ�����ֵ��0.02�������������������ڼ�/�Ÿ��ڼ䲻����
                // NOTE(2026-07-19): threshold restore moved to the safety-net check below

                // idleʱ������˷����棨37.5dB����������װλ������
                Application::GetInstance().GetAudioService().SetInputGain(45.0f);
            }
            sm->prev_device_state_ = dev_state;

            // Safety net (2026-07-19): whenever device is in quiet idle (no show,
            // no music), enforce sensitive wake threshold 0.02. Fixes paths that
            // leaked 0.30: session ended during music, show finished while idle, etc.
            // Flag ensures we only set once per quiet-idle entry (no log spam).
            static bool idle_thresh_applied = false;
            bool idle_quiet = (dev_state == (int)kDeviceStateIdle) && !sm->IsRunning();
            if (idle_quiet) {
                if (!idle_thresh_applied) {
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.02f);
                    idle_thresh_applied = true;
                }
            } else {
                idle_thresh_applied = false;
            }

            // AI˵��ʱС����滰��������Ծ�����ڱ��������ޱ�������ʱ��
            if (dev_state == (int)kDeviceStateSpeaking) {
                // �����Ƶ�����Ծ�ȣ�300ms ������� = ����˵����
                int64_t ms_since_output = Application::GetInstance().GetAudioService().MsSinceLastOutput();
                if (ms_since_output < 300) {
                    // ����˵����������������50-200ms�����
                    sm->BirdJumpShort();
                }
                // ��˵��ʱ����
            }
             sm->MusicDanceTick();
        }

        // === ÿ��（sub_tick % 4 == 0）===
        if (sub_tick % 4 == 0) {
            tick_sec = sub_tick / 4;

        // NTPͬ����δͬ��ʱÿ60�����ԣ�ͬ����ÿ300��У׼һ��
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

        // WiFiʡ����CuckooBoard::SetPowerSaveLevel���ش���
        // �����ֲ���ʱ����LOW_POWER��

 // ÿ30�뱣�������־��RTC�������ڴ棨RTC�ڿ��Ź���λ���Ա�����
        if (tick_sec % 30 == 0) {
            auto& app = Application::GetInstance();
            rtc_crash_log.tick_sec = tick_sec;
            rtc_crash_log.dev_state = (uint8_t)app.GetDeviceState();
            rtc_crash_log.music_active = (uint8_t)app.GetAudioService().IsBgAudioActive();
            rtc_crash_log.free_heap = esp_get_free_heap_size();
        }

 // ÿ60������+ջˮλ�����ڱ������
        if (tick_sec % 60 == 0) {
            ESP_LOGI(TAG, "Heartbeat t=%ds stack_hwm=%d free_heap=%d",
                     (int)tick_sec, (int)uxTaskGetStackHighWaterMark(NULL),
                     (int)esp_get_free_heap_size());
        }

    // ��Ƶ��Դ������������/������� 15 ���ر� I2S ���»���ʧЧ
        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();
            // ��ȡ�����������ҹ��ģʽ
            sm->is_dark_ = sm->CheckDark();
        }

        // ���ʱ����ͨ�� NTP �� MCP ���ã����ڲ�ʱ��
        if (sm->time_set_) {
            // NTP��ͬ��ʱ��ϵͳʱ�䣬��������ʱ��Ư��
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
            } else {
                // NTPδͬ������MCP���ʱ�䣩�����˵�����ʱ�ӵ���
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

            // ����Ͱ����
            int h = sm->current_hour_;
            int m = sm->current_min_;

        // ���㱨ʱÿ���� == 0 ʱ�������� NeedHourlyChime ���ظ���
            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }
        // ��㱨ʱÿ���� == 30 ʱ�������� NeedHalfHourlyChime ���ظ���
            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }

            // �����飨ÿ����һ�Σ�
            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        }  // sub_tick >= 4 guard
 // ע��NTPͬ��������ʱ������ÿ��tick_sec%86400У׼
    }
}
