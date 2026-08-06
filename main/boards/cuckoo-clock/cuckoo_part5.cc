// ===== Part 5: MCP注册、clock_task (L4413-4991) =====

/**
 * @brief 音乐舞蹈 tick（250ms 主循环调用）
 * 音乐播放且小朋友们活跃时：M1 正反转脉冲 + 吉他/狗尾舵机摆动；
 * 音乐停止或小朋友休息时：平衡 M1、送狗退场、关门。
 */
void CuckooStateMachine::MusicDanceTick() {
    // Mp3Player 跟踪播放状态（AI 说话 ducking 期间保持 true），
    // 而 IsBgAudioActive() 在音频服务清空缓冲时可能短暂为 false。
    // 两者并用确保音乐舞蹈在 AI 对话期间不中断。
    bool music_playing = (mp3_ && mp3_->IsPlaying()) ||
                         Application::GetInstance().GetAudioService().IsBgAudioActive();
    // 跟踪 kids_active_ 状态变化，用于音乐中途的切换
    static bool was_kids_active = true;
    bool kids_now = kids_active_.load();

    if (music_playing && !IsRunning()) {
        // MusicDanceTick 日志静音以降低噪声（每 270ms 打印一次）
        if (music_dance_enabled_ != 1) {
            music_dance_phase_ = 0;
            music_dance_enabled_ = 1;
            dog_outro_done_ = false;
            was_kids_active = kids_now;
        }

        // 音乐播放且小朋友们活跃时小狗出场。不受 music_dance_enabled_
        // 初始化块门控（该块可能被陈旧状态跳过）。
        if (kids_now && !dog_intro_running_ && !dog_intro_done_) {
            dog_intro_running_ = true;
            MusicDogIntro();
        }

        // 音乐中途切换：小朋友们变为活跃 → 小狗出场
        if (!was_kids_active && kids_now && !dog_intro_running_ && !dog_intro_done_) {
            was_kids_active = true;
            dog_outro_done_ = false;
            dog_intro_running_ = true;
            MusicDogIntro();
        }
        // 音乐中途切换：小朋友们变为休息 → 停止并送回小狗
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

        // 等待小狗出场动画结束再接管舵机/电机
        if (kids_now && dog_intro_done_) {
            if (kids_appreciating_ && m1_) {
                // Phase 0-3 正转 20ms，Phase 4-5 反转 20ms，Phase 6 反转 22ms，Phase 7 反转 24ms
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
            
            // 吉他 + 小狗舵机：与同一节奏同步
            // Phase 0-3：正向节拍，Phase 4-7：反向节拍
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
        // 演出进行中（ShowTask/KidsDanceShow）不关闭设备
        if (is_running_) return;
        // MusicDT-ELSE 日志静音以降低噪声（每 250ms 打印一次）
        was_kids_active = kids_now;
        if (music_dance_enabled_ == 1) {
            // 平衡 M1 电机：反转次数必须匹配正转次数
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
            // 音乐结束：小狗回去、关门（仅当小朋友们活跃过）
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

/**
 * @brief 小狗出场：开门（异步）+ 狗尾 180→20→35 度 + 小狗前进
 */
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
    // 重新打开电机电源（防止 MusicDogOutro 中途关闭了它）
    MotorPowerOn();
    dog_intro_done_ = true;
    dog_intro_running_ = false;
    dog_out_ = true;
    ESP_LOGI(TAG, "MusicDogIntro: done, handing over to MusicDanceTick");
}

/**
 * @brief 小狗退场：狗尾回 20→180 度、小狗后退、关门、关电机电源
 */
void CuckooStateMachine::MusicDogOutro() {
    dog_outro_running_ = true;
    if (!dog_out_.load()) {
        ESP_LOGI(TAG, "MusicDogOutro: dog was not out, skip all motors");
        dog_outro_running_ = false;
        return;
    }
    dog_out_ = false;
    // 先平滑回到 30 度，再回原位
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
    if (door_open_.load()) {
        CloseDoor();
    } else {
        ESP_LOGI(TAG, "MusicDogOutro: door already closed, skip motor");
    }
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
    // 立即置 kids_active_，避免 MusicDanceTick 进入 else 分支
    // 而在下面开门延时期间触发 MusicDogOutro
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
        // 可见舞蹈：与整点报时相同的大幅动作
        sm->RunDanceLoop();
        // MusicDanceTick 恢复守卫：停 M1 并重置状态，防止歌曲中途竞态
        if (sm->m1_) sm->m1_->Stop();
        sm->music_dance_enabled_ = 0;
        sm->m1_music_fwd_count_ = 0;
        sm->m1_music_rev_count_ = 0;
        if (sm->kids_dance_) {
            bool music_stopped = !Application::GetInstance().GetAudioService().IsBgAudioActive()
                                 && (!sm->mp3_ || !sm->mp3_->IsPlaying());
            if (music_stopped) {
                // 音乐结束：完整清理，全部关闭
                sm->kids_appreciating_ = false;
                gpio_set_level(LED_A_GPIO, 0);
                gpio_set_level(LED_B_GPIO, 0);
                if (sm->water_bird_) sm->water_bird_->Stop();
                sm->MusicDogOutro();
                sm->kids_active_ = false;
            } else {
                // 音乐仍在播放：平滑过渡到轻摆
                sm->kids_appreciating_ = true;
                gpio_set_level(LED_A_GPIO, 0);
                gpio_set_level(LED_B_GPIO, 0);
            }
            sm->kids_dance_ = false;
        }
        // else：用户提前停止，保持开门 → MusicDanceTick 接管
        sm->is_running_ = false;
        ESP_LOGI(TAG, "KidsDanceShow: dance finished, cleaned up");
        vTaskDelete(NULL);
    }, "kids_dance", 4096, this, 4, NULL);
}

/**
 * @brief 小朋友们出来欣赏音乐（MCP cuckoo.kids_come_out）
 * 置 kids_active_ 并保存；若音乐在播且小狗未出场则强制创建小狗出场。
 */
void CuckooStateMachine::KidsComeOut() {
    kids_active_ = true;
    dog_out_ = true;
    kids_appreciating_ = true;  // light M1 swinging via MusicDanceTick
    SaveKidsActive();
    ESP_LOGI(TAG, "KidsComeOut: kids active, will dance with music");
    // 安全措施：音乐在播但小狗还没出场时，强制创建小狗出场任务。
    // 绕过 MusicDanceTick 状态机（它可能因 Core 1 上 prio-6 任务调度竞态
    // 在 dog_outro 与 dog_intro 之间漏掉小狗）。
    // 用户明确要求小朋友 → 不管陈旧状态标志，强制创建小狗出场。
    // 不检查 dog_intro_done_ 或 dog_intro_running_（它们可能被
    // MusicDanceTick 与 PerformanceTask 的竞态条件污染）。
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

/**
 * @brief 小朋友们回去休息：停止舞蹈与轻摆，平衡 M1，送回小狗并关门
 */
void CuckooStateMachine::KidsRest() {
    if (kids_dance_) { ESP_LOGI(TAG, "KidsRest: stopping dance, sending kids back"); is_running_ = false; kids_dance_ = false; /* force RunDanceLoop exit, then fall through to close */ }
    kids_active_ = false;
    kids_appreciating_ = false;  // stop M1 light swinging
    SaveKidsActive();
    // 平衡 M1 电机：停止前反转次数必须匹配正转次数
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
    // 停止所有动作
    if (m1_) m1_->Stop();
    if (violin_servo_) violin_servo_->SetAngle(90);
    if (dog_servo_) dog_servo_->SetAngle(dog_state_.angle);  // hold current position
    // 立即送回小狗并关门
    MusicDogOutro();
    dog_outro_done_ = true;  // prevent duplicate cleanup when music ends
    ESP_LOGI(TAG, "KidsRest: kids resting, dog back, door closing");
}

/**
 * @brief 打开鸟门（M4 正转固定时长）
 */
void CuckooStateMachine::OpenBirdDoor() {
    MotorPowerOn();
    if (m4_) {
        m4_->Forward(BIRD_DOOR_OPEN_SPEED);
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
    MotorPowerOff();
}

/**
 * @brief 关闭鸟门（M4 反转固定时长）
 */
void CuckooStateMachine::CloseBirdDoor() {
    MotorPowerOn();
    if (m4_) {
        m4_->Reverse(BIRD_DOOR_CLOSE_SPEED);
        vTaskDelay(pdMS_TO_TICKS(BIRD_DOOR_TIME_MS));
        m4_->Stop();
    }
    MotorPowerOff();
}

/**
 * @brief 启动 LED 交替闪烁任务（300ms 间隔，双灯交替）
 */
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

/**
 * @brief 停止 LED 闪烁任务
 */
void CuckooStateMachine::StopFlashLeds() {
    flash_leds_active_ = false;
}

/**
 * @brief 播放布谷鸟叫声（同步，开电机电源）
 */
void CuckooStateMachine::PlayCuckooSound() {
    MotorPowerOn();
    if (bell_player_) {
        bell_player_->PlayCuckooSoundSync();
    }
    MotorPowerOff();
}

/**
 * @brief 设置舵机角度（MCP 工具）
 */
void CuckooStateMachine::SetServoAngle(int servo_id, int angle) {
    MotorPowerOn();
    if (servo_id == 0 && violin_servo_) violin_servo_->SetAngle(angle);
    else if (servo_id == 1 && dog_servo_) dog_servo_->SetAngle(angle);
}

/**
 * @brief 设置电机速度（MCP 工具）
 */
void CuckooStateMachine::SetMotorSpeed(int motor_id, int speed) {
    if (speed != 0) MotorPowerOn();
    switch (motor_id) {
        case 1: if (m1_) m1_->SetSpeed(speed); break;
        case 2: if (violin_motor_) violin_motor_->SetSpeed(speed); break;
        case 3: if (m3_) m3_->SetSpeed(speed); break;
        case 4: if (m4_) m4_->SetSpeed(speed); break;
        default: break;
    }
    if (speed == 0) MotorPowerOff();
}

/**
 * @brief 设置鸟门电机速度（MCP 工具）
 */
void CuckooStateMachine::SetBirdDoorSpeed(int speed) {
    if (speed != 0) MotorPowerOn();
    if (m4_) m4_->SetSpeed(speed);
    if (speed == 0) MotorPowerOff();
}

/**
 * @brief 粤语查询：经音乐代理返回拼音+释义（保留接口，已不注册为 MCP 工具）
 */
std::string CuckooStateMachine::CantoneseLookup(const char* word) {
    if (music_proxy_host_.empty()) {
        ESP_LOGW(TAG, "CantoneseLookup: proxy not configured");
        return "";
    }

    // 手工对单词做 URL 编码（供 esp_http_client 使用）
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

    // 读取响应体
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
 * @brief 检查多版本歌曲：服务器返回歌手列表 JSON 则返回给 AI 念给用户选
 * @param url_or_path 代理路径
 * @return 非空=多版本 JSON（应念给用户选）；空=单曲直接播
 */
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

    // 用原始 socket HTTP/1.0，避免 esp_http_client 的 HTTP/1.1
    // （仅检查请求也会让服务器启动 ffmpeg）
    struct hostent* he = gethostbyname(music_proxy_host_.c_str());
    if (!he) return "";
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(music_proxy_port_);
    addr.sin_addr.s_addr = *(uint32_t*)he->h_addr;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    int timeout_ms = 8000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(sock); return "";
    }

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        encoded_path, music_proxy_host_.c_str(), music_proxy_port_);
    send(sock, req, strlen(req), 0);

    // 读取 HTTP 响应头
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
    if (status != 200) {
        // 读取错误响应体（404 时服务器返回带提示和备选曲目的 JSON）
        int content_len = 0;
        const char* cl = strstr(header_buf, "Content-Length:");
        if (!cl) cl = strstr(header_buf, "content-length:");
        if (cl) sscanf(cl + 15, "%d", &content_len);
        if (content_len > 0 && content_len < 4096) {
            char* resp = (char*)calloc(1, content_len + 1);
            if (resp) {
                int total = 0;
                while (total < content_len) {
                    int n = recv(sock, resp + total, content_len - total, 0);
                    if (n <= 0) break;
                    total += n;
                }
                resp[total] = '\0';
                if (total > 0) {
                    std::string result(resp);
                    free(resp);
                    closesocket(sock);
                    return result;
                }
                free(resp);
            }
        }
        closesocket(sock);
        return "{\"error\": \"song_not_found\", \"status\": " + std::to_string(status) + "}";
    }

    // 解析 Content-Length
    int content_len = 0;
    const char* cl = strstr(header_buf, "Content-Length:");
    if (!cl) cl = strstr(header_buf, "content-length:");
    if (cl) sscanf(cl + 15, "%d", &content_len);
    ESP_LOGI(TAG, "CheckMultiArtist: status=%d content_len=%d", status, content_len);

    if (content_len <= 0) { closesocket(sock); return ""; }

    // 读取响应体
    char* resp = (char*)calloc(1, content_len + 1);
    if (!resp) { closesocket(sock); return ""; }
    int total = 0;
    while (total < content_len) {
        int n = recv(sock, resp + total, content_len - total, 0);
        if (n <= 0) break;
        total += n;
    }
    closesocket(sock);
    resp[total] = '\0';

    if (total > 0 && resp[0] == '{' && strstr(resp, "multi_artist")) {
        ESP_LOGI(TAG, "CheckMultiArtist: detected multi-artist, %d bytes", total);
        std::string result(resp);
        free(resp);
        return result;
    }
    free(resp);
    return "";
}

/**
 * @brief 播放在线音乐（走 PlayOpus：/opus 或 /pcm 双路径）
 * @param url_or_path 完整 URL 或代理路径（/pcm?q=歌名 等）
 * @return >=0 成功；-1 播放器未初始化；-10 代理未配置
 * 自动做 URL 编码、路径转换；已在播放时先停旧任务再播新歌。
 */
int CuckooStateMachine::PlayOnlineMusic(const char* url_or_path) {
    if (!mp3_) return -1;
    
    auto& app = Application::GetInstance();
    auto ai_state = app.GetDeviceState();
    if (ai_state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "PlayOnlineMusic: AI speaking, music will overlap (ducked)");
    }
    
    // 若正在播放，先干净地停止旧任务再播新的
    
    // 从 URL 路径自动检测格式
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
        ConvertToPcmUrl(conv_url, sizeof(conv_url));
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
        return mp3_->PlayOpus(conv_url);
    }
    
    if (music_proxy_host_.empty()) {
        ESP_LOGE(TAG, "Music proxy not configured. Check DEFAULT_MUSIC_PROXY_HOST in config.h.");
        return -10;
    }
    
 // 对非 ASCII 字符做 URL 编码（中文等），esp_http_client 不支持原始中文 URL
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
    
    char full_url[1280];  // http:// + host + :port + encoded_path
 // 确保路径以 / 开头
    if (encoded_path[0] != '/') {
        snprintf(full_url, sizeof(full_url), "http://%s:%d/%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    } else {
        snprintf(full_url, sizeof(full_url), "http://%s:%d%s",
                 music_proxy_host_.c_str(), music_proxy_port_, encoded_path);
    }
    ConvertToPcmUrl(full_url, sizeof(full_url));
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
 * @brief 设置 QQ 音乐代理服务器（MCP 工具）
 */
void CuckooStateMachine::SetMusicProxy(const char* host, int port) {
    music_proxy_host_ = host;
    music_proxy_port_ = port;
    ESP_LOGI(TAG, "Music proxy set to %s:%d", host, port);
}

// ============================================
// ============================================
/**
 * @brief 构造 MCP 工具集，绑定状态机指针
 */
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

/**
 * @brief 注册全部 MCP 工具（cuckoo.* 前缀，供 AI 大模型调用）
 * 包括：报时、时间、音乐、演出、门/灯、闹钟、安静模式、在线音乐、各角色秀等。
 */
void CuckooTools::RegisterAll() {
    auto& mcp = McpServer::GetInstance();

 // === 时间 / 报时 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.performance",
            "Hourly chime: bell rings + music. ONLY call when user explicitly says 报时/半点报时/几点/what time. DO NOT auto-call on wake-up. For shows/singing/dancing use cuckoo.start_show instead.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int hour = props["hour"].value<int>();
                state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
                return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
            });
    }
    // === LED 闪烁 (2026-07-29) ===
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

 // === 音乐 / 演出 ===
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
        "综合表演：舞蹈+小提琴+水车+鸟跳一起进行。触发词 '表演' '节目' '出来表演' '演出' '表演' 等综合表演相关。"
        "注意：用户只提到某个角色（花园/美边/小狗）时不要用此工具，用对应的角色工具。识别到文本中包含角色（'花园''琳达''小狗' ）时也用对应角色工具，不要用此工具。",
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
        "Stop music playback. Call ONLY when user explicitly asks to stop the music (停止/别播了/不要音乐/关掉). Do NOT call this for performance or alarm - use cuckoo.stop_all for those.",
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

 // === 硬件（稍后接线）===
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


    // === 闹钟 ===
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
        "Stop a ringing ALARM only. For 关闭闹钟/停闹钟. NOT for stopping music/performance - use cuckoo.stop_all for that.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === 安静模式 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("mode", kPropertyTypeInteger, 0, 3));
        pl.AddProperty(Property("start_hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("end_hour", kPropertyTypeInteger, 0, 23));
        mcp.AddTool("cuckoo.set_quiet_mode",
            "Set chime quiet mode. 0=全天静音(永不报时), 1=全天报时, 2=暗光静音(LDR判断), 3=指定时间段静音(start_hour~end_hour之间). "
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
                m == 0 ? "全天静音" : m == 1 ? "全天报时" :
                m == 2 ? "暗光静音(LDR)" : "时间段静音");
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

    // === 整点报时演出开关 ===
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
//
// ============================================
/**
 * @brief 布谷鸟钟主循环任务（Core 1，250ms tick）
 * 处理：NTP 时钟同步与报时触发、设备状态变化（开门/关门）、
 * 音乐舞蹈 tick、闹钟检查、心跳日志与崩溃日志保存、LDR 暗光刷新。
 */
void cuckoo_clock_task(void* params) {
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

    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();


    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1000000000) {
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

    // Bug 修复：NTP 同步可能跳过了报时边界。
    // 立即检查是否需要触发整点/半点报时。
    if (sm->time_set_.load()) {
        int h = sm->current_hour_.load();
        int m = sm->current_min_.load();
        if (m == 0 && sm->NeedHourlyChime(h)) {
            ESP_LOGI(TAG, "NTP init crossed hourly boundary, triggering chime for %02d:00", h);
            sm->MarkHourlyChime(h);
            sm->CheckTime(h, m, sm->is_dark_.load());
        } else if (m == 30 && sm->NeedHalfHourlyChime(h)) {
            ESP_LOGI(TAG, "NTP init crossed half-hour boundary, triggering chime for %02d:30", h);
            sm->MarkHalfHourlyChime(h);
            sm->CheckTime(h, m, sm->is_dark_.load());
        }
    }

    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);

    sm->LoadAlarmsFromNvs();
    sm->LoadQuietMode();
    sm->LoadKidsActive();

uint32_t tick_sec = 0;

    uint32_t sub_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        sub_tick++;

        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, "Device woke up - opening bird door");
                sm->OpenBirdDoor();

                if (!sm->IsRunning())
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.3f);

                Application::GetInstance().GetAudioService().SetInputGain(30.0f);
            } else if (sm->prev_device_state_ != (int)kDeviceStateIdle && dev_state == (int)kDeviceStateIdle) {
                ESP_LOGI(TAG, "Device sleeping - closing bird door");
                sm->CloseBirdDoor();

                // NOTE(2026-07-19)：阈值恢复移到下面安全网检查中

                Application::GetInstance().GetAudioService().SetInputGain(45.0f);
            }
            sm->prev_device_state_ = dev_state;

            // 安全网 (2026-07-19)：设备处于安静空闲（无演出、
            // 无音乐）时强制恢复灵敏唤醒阈值 0.02。修复了泄漏 0.30 的路径：
            // 音乐中会话结束、演出在空闲时结束等。
            // 标志确保每次进入安静空闲只设置一次（避免日志刷屏）。
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

            if (dev_state == (int)kDeviceStateSpeaking) {
                int64_t ms_since_output = Application::GetInstance().GetAudioService().MsSinceLastOutput();
                if (ms_since_output < 300 && !sm->IsRunning()) {
                    sm->BirdJumpShort();
                }
            }
             sm->MusicDanceTick();
        }

        if (sub_tick % 4 == 0) {
            tick_sec = sub_tick / 4;

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

                // Bug 修复：周期性 NTP 同步可能跳过了报时边界。
                int h = sm->current_hour_.load();
                int m = sm->current_min_.load();
                if (m == 0 && sm->NeedHourlyChime(h)) {
                    ESP_LOGI(TAG, "NTP sync crossed hourly boundary, triggering chime for %02d:00", h);
                    sm->MarkHourlyChime(h);
                    sm->CheckTime(h, m, sm->is_dark_.load());
                } else if (m == 30 && sm->NeedHalfHourlyChime(h)) {
                    ESP_LOGI(TAG, "NTP sync crossed half-hour boundary, triggering chime for %02d:30", h);
                    sm->MarkHalfHourlyChime(h);
                    sm->CheckTime(h, m, sm->is_dark_.load());
                }
            } else {
                ESP_LOGW(TAG, "NTP not synced yet, will retry in 60s");
            }
        }


        if (tick_sec % 30 == 0) {
            auto& app = Application::GetInstance();
            rtc_crash_log.tick_sec = tick_sec;
            rtc_crash_log.dev_state = (uint8_t)app.GetDeviceState();
            rtc_crash_log.music_active = (uint8_t)app.GetAudioService().IsBgAudioActive();
            rtc_crash_log.free_heap = esp_get_free_heap_size();
        }

        if (tick_sec % 60 == 0) {
            ESP_LOGI(TAG, "Heartbeat t=%ds stack_hwm=%d free_heap=%d",
                     (int)tick_sec, (int)uxTaskGetStackHighWaterMark(NULL),
                     (int)esp_get_free_heap_size());
        }

        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();
            sm->is_dark_ = sm->CheckDark();
        }

        if (sm->time_set_) {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
            } else {
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

            int h = sm->current_hour_;
            int m = sm->current_min_;

            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }
            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                sm->CheckTime(h, m, sm->is_dark_);
            }

            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        }  // sub_tick >= 4 guard
    }
}
