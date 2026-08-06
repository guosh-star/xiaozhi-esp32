/**
 * @brief 通过原始 BSD Socket 播放网络音乐（Opus OGG 或原始 PCM 流）
 * @param url 音乐 URL（http://host:port/path/...）
 * @return 0=成功启动；-1=已在播放或 URL 过长
 * - 启动 PlayOpusTask 后台任务下载并播放；播放前清空遗留背景音频、重置停止标志与 ducking
 * - Opus 路径：OGG 解封装 → PushPacketToDecodeQueue（原 Opus 解码队列）
 * - PCM 路径：legacy 模式 PushBackgroundAudio（高保真音频 ring buffer）
 * - Ducking：AI 说话时暂停推送数据，让出带宽给语音，说完恢复
 * - 自动将音乐音量降到 65%，用于 AEC 参考采集降噪
 * - 网络不通时，降级到串口回退模式
 */
int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        ESP_LOGW(TAG, "PlayOpus: music already playing, refusing (call stop_music first)");
        return -1;
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
    stop_requested_ = false;  // 每次播放前重置停止标志
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;

    // 清除上一会话遗留的背景音频，防止启动时噪声突发
    Application::GetInstance().GetAudioService().ClearBackgroundAudio();

    if (strlen(url) >= 512) {
        ESP_LOGE(TAG, "PlayOpus: URL too long (%u bytes)", (unsigned)strlen(url));
        return -1;
    }
    struct OpusCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new OpusCtx();
    ctx->self = this;
    strncpy(ctx->url, url, 511);
    ctx->url[511] = '\0';

    xTaskCreatePinnedToCore(PlayOpusTask, "opus_http", 1024 * 24, ctx, 3, NULL, 1);  // OGG/Opus 解码需要 24KB 栈
    return 0;
}

/**
 * @brief 网络音乐下载播放任务（后台线程，栈 24KB）
 * 建立 TCP 连接发送 HTTP GET，按路径分流：
 * - /opus：OGG 解复用器 + PushPacketToDecodeQueue（与 AI 语音共用解码队列），音量降至 65% 供 AEC；
 * - /pcm：原始 s16le 推入背景音频环形缓冲，预缓冲满 64000 样本后开启排空；
 * 支持串口回退（网络不通时经 UART0 转发）；AI 说话时 ducking 暂停下载/降音量；
 * 播放结束恢复音量与唤醒词阈值，必要时重启唤醒词检测。
 */
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

 // 使用背景音频播放，Ducking 控制 AI 语音

    // 解析 URL
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
    
    // 原始BSD socket
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
 // 先尝试点分十进制 IP，再尝试 DNS 解析
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
    
 // 非阻塞 connect + 5 秒超时，SO_SNDTIMEO 对 lwip 不生效
 // 防止连接超时直接 goto serial_fallback，未关闭 sock
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
        fcntl(sock, F_SETFL, sock_flags);  // 恢复阻塞模式
    }
    
    if (false) {  // 串口回退开关
serial_fallback:
 // 网络路径不通，goto 到这里，sock 已关闭
        
 // === 串口回退 ===
        printf("\x01MUSIC_REQ\x02%s\x03\n", url);
        fflush(stdout);
        
 // 复位停止标志，使 PlayOpus 中 Stop() 的调用不会中断 fread 循环
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
        
 // 从 UART0 RX 读取 Opus 数据，开始延时等待 serial_relay 转取数据
        vTaskDelay(pdMS_TO_TICKS(3000));  // 等待 3 秒让 serial_relay 转取数据
        
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
 // 防止 AFE 音频源超时被 watchdog 误杀
                uint64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_refresh_ms > 8000) {  // 每 8 秒
 // HACK: 防止长时间下载期间音频看门狗超时
                app.GetAudioService().RefreshInputTimestamp();

                    last_refresh_ms = now_ms;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            uint64_t now = esp_timer_get_time() / 1000;
            if (stream_started && (now - last_data_ms > 5000)) break;  // 5 秒空闲
            if (!stream_started && (now - serial_start > 15000)) break;  // 15 秒启动超时
            if (now - serial_start > 120000) break;  // 总计 2 分钟
            if (app.GetDeviceState() == kDeviceStateConnecting) break;  // 断连词唤醒后，停止下载
 // 500ms 双重检测，防止说话声被误判为断连而停止
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
    
    // 发送 HTTP GET 请求
    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
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
    ESP_LOGI(TAG, "PlayOpus: HTTP %d", status);
    
    if (status != 200) {
        ESP_LOGE(TAG, "PlayOpus: HTTP error %d", status);
        close(sock);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }

 // === 检测格式：/opus 使用 OGG 解复用器 + 主音频管线 ===
 // === /pcm 使用原始字节推入后台环形缓冲 ===
    bool use_opus = (strstr(path, "/opus") != nullptr);
    
    if (use_opus) {
        // ============ Opus 路径：OGG 解复用 + PushPacketToDecodeQueue ============
 // 与 AI 语音 Opus 播放共用同一解码队列
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
        
 // 播放音乐时音量降至 65%，并开启 AEC 参考采集
        auto* codec = Board::GetInstance().GetAudioCodec();
        int old_vol = codec ? codec->output_volume() : 90;
        int music_vol = old_vol * 65 / 100;
        if (music_vol < 10) music_vol = 10;
        if (codec) codec->SetOutputVolume(music_vol);
        ESP_LOGI(TAG, "PlayOpus: volume %d -> %d for AEC", old_vol, music_vol);
        
        bool is_ducking = false;  // true = AI 正在说话，不推送音乐
        
        while (self->is_playing_ && !self->stop_requested_) {
            int read = recv(sock, buf, CHUNK, 0);
            if (read <= 0) {
                ESP_LOGW(TAG, "PlayOpus: recv returned %d errno=%d", read, (read < 0) ? errno : 0);
                break;
            }
            total_dl += read;

 // Ducking: AI 说话时暂停下载数据，让出 WiFi 带宽给语音
 // 仅在 speaking 状态时 ResetDecoder，避免清除私有数据
 // 若先清除数据会使播放器前端丢一小段（5-15ms），AI 重复中间内容
 // 先等 3-5 秒的音频，再 5-15 秒 AI 重复中间内容
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
                demuxer->Process(buf, read);  // 保持解码器状态，跳过输出
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
        
        // 恢复音量
        if (codec) codec->SetOutputVolume(old_vol);
        ESP_LOGI(TAG, "PlayOpus: volume restored to %d", old_vol);
        
 // 等待播放队列排空
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
    
 // ============ PCM 路径（旧版）：原始 s16le PushBackgroundAudio ============
    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    int64_t t0 = esp_timer_get_time(), last_refresh = t0;
    size_t total_dl = 0;
    bool ai_speaking = false;
    int prev_ai_level = 0;   // 0=空闲, 1=聆听, 2=说话/连接中

    ESP_LOGI(TAG, "PlayOpus: downloading raw PCM...");

    // 预缓冲循环前先激活环形缓冲，PushBackgroundAudio 才能生效。
    // 否则 bg_audio_active_ 保持 false（PlayOpus 中 ClearBackgroundAudio 所致）
    // 预缓冲循环会静默丢弃所有数据，直到 10 秒超时。
    app.GetAudioService().SetBackgroundAudioGain(0.001f);  // 近静音但保持 bg_audio_active_=true

 // === 预缓冲阶段：先填满环形缓冲再开启排空 ===
    int64_t prebuf_start = esp_timer_get_time();
    while (self->is_playing_ && !self->stop_requested_) {
        int read = recv(sock, buf, CHUNK, 0);
        if (read <= 0) break;
        total_dl += read;
        app.GetAudioService().PushBackgroundAudio(
            reinterpret_cast<int16_t*>(buf), read / sizeof(int16_t), 16000);

        size_t fill = app.GetAudioService().GetBgAudioFillLevel();
        if (fill >= 64000) {
            app.GetAudioService().SetBackgroundAudioGain(1.0f);  // 通过约 1000ms 斜坡淡入
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGI(TAG, "PlayOpus: drain enabled (buffered %d samples in %d ms)",
                     (int)fill, (int)((esp_timer_get_time() - prebuf_start) / 1000));
            break;
        }
        if (esp_timer_get_time() - prebuf_start > 10000000) {
            auto state = app.GetDeviceState();
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            prev_ai_level = ai_now ? 2 : 0;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGW(TAG, "PlayOpus: pre-buffer timeout after 10s (%d samples buffered)",
                     (int)fill);
            break;
        }
    }

 // === 主下载推送循环 ===
    int recv_errors = 0;
    while (self->is_playing_ && !self->stop_requested_) {
        auto state = app.GetDeviceState();
        // 三档 ducking：空闲=100%，聆听=70%，说话/连接=50%
        int ai_level = 0;
        if (state == kDeviceStateSpeaking || state == kDeviceStateConnecting) {
            ai_level = 2;
        } else if (state == kDeviceStateListening) {
            ai_level = 1;
        }
        // 跟踪 ai_speaking 供 WiFi 节流逻辑使用（仅 level 2）
        bool ai_now = (ai_level == 2);
        if (ai_now != ai_speaking) {
            ai_speaking = ai_now;
        }
        if (ai_level != prev_ai_level) {
            prev_ai_level = ai_level;
            float gain = (ai_level == 2) ? 0.5f : 1.0f;  // 说话时降到 50% 防止混音削波
            app.GetAudioService().SetBackgroundAudioGain(gain);
            int heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            int bg_fill = app.GetAudioService().GetBgAudioFillLevel();
            int task_hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "PlayOpus: AI %s, music -> %d%%, heap=%d sram=%d bg_buf=%d task_hwm=%d",
                     ai_level == 2 ? "speaking" : (ai_level == 1 ? "listening" : "idle"),
                     (int)(gain * 100),
                     heap_free / 1024, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     bg_fill, task_hwm);
        }

 // AI 活跃（聆听/说话/连接）时暂停 TCP 下载
 // 释放 lwIP 缓冲给 UDP 音频；否则 ASR 需要 30-50 秒。
        if (ai_level >= 1 && app.GetAudioService().GetBgAudioFillLevel() > 64000) {
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
 // 服务器正常关闭连接
            ESP_LOGI(TAG, "PlayOpus: server closed connection");
            break;
        } else {
 // read < 0：超时或瞬时错误
            recv_errors++;
            if (recv_errors > 5) {
                ESP_LOGE(TAG, "PlayOpus: recv failed %d times, giving up (errno=%d)", recv_errors, errno);
                break;
            }
 // 重试前等待 1 秒，AI 对话后 WiFi 可能正在重连
            ESP_LOGW(TAG, "PlayOpus: recv error %d/%d (errno=%d), retrying...", recv_errors, 5, errno);
            for (int w = 0; w < 10 && self->is_playing_ && !self->stop_requested_; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
 // 刷新省电设置（通道关闭可能已改变它）
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            continue;
        }

        while (app.GetAudioService().GetBgAudioFillLevel() > 80000 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_refresh > 8000000) {
 // 诊断：每 8 秒记录缓冲填充量和下载速率
            size_t fill = app.GetAudioService().GetBgAudioFillLevel();
            int64_t elapsed = now - t0;
            float dl_rate = elapsed > 0 ? (float)total_dl * 1000000.0f / (float)elapsed : 0;
            bool duck_now = (app.GetDeviceState() == kDeviceStateSpeaking || 
                            app.GetDeviceState() == kDeviceStateConnecting);
            int heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            int sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            int task_hwm = uxTaskGetStackHighWaterMark(NULL);
            bool aec_active = true;
            ESP_LOGI(TAG, "PlayOpus: buf=%d dl=%dKB rate=%.1fKB/s duck=%d%% heap=%dKB sram=%d task_hwm=%d aec=%d",
                     (int)fill, (int)(total_dl / 1024), dl_rate / 1024.0f, duck_now ? 50 : 100,
                     heap_free / 1024, sram_free, task_hwm, aec_active ? 1 : 0);
            app.GetAudioService().RefreshInputTimestamp();
            last_refresh = now;
        }
    }

    if (total_dl > 0) {
        // 直接清空，不等 ring buffer 排空
        // 原来 while 等待排空会导致 MusicDanceTick 继续跑 ~5 秒（79K 样本 ÷ 16kHz）
        app.GetAudioService().ClearBackgroundAudio();
    }
    close(sock);
    self->is_playing_ = false;
    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "PlayOpus: done %dms, dl=%dKB", (int)total_ms, (int)(total_dl / 1024));

    // 设备空闲时恢复唤醒词检测，确保演出正常通过扬声器
    // 定时背景音频信号，若音频输出路径异常则停止
    // 用 esp_codec_dev_read 回调检测/传输数据的完整流程
    // 唤醒词检测结束后的“演出”真实状态
    if (app.GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "PlayOpus: restarting wake word detection after music");
        app.GetAudioService().EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        app.GetAudioService().EnableWakeWordDetection(true);
        // 修复(2026-07-19)：音乐期间进入空闲会跳过阈值恢复
        // （IsBgAudioActive 守卫）导致 0.30 卡住，在此恢复灵敏阈值 0.02。
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 播放闹钟铃声（A5/C#6 交替双音：80ms 响 + 80ms 停，共 2 秒）
 * @param volume 音量系数 0.0~1.0
 * 生成 A5(880Hz)/C#6(1100Hz) 正弦波 PCM，每 100ms 分块经 OutputRawPcm 输出，
 * 块间检查 stop_requested_ 保证停止响应及时。
 */
void Mp3Player::PlayAlarmRing(float volume) {
    auto& app = Application::GetInstance();
    const int sample_rate = 16000;
    const int chunk_ms = 100;  // 100ms 分块，保证停止响应及时
    const int total_ms = 2000;
    const int chunk_samples = sample_rate * chunk_ms / 1000;
    const int total_chunks = total_ms / chunk_ms;

    const int freq_a = 880;   // A5
    const int freq_b = 1100;  // C#6
    const int base_amplitude = 8000;
    const int beep_on_ms = 80;
    const int beep_off_ms = 80;
    const int cycle_ms = beep_on_ms + beep_off_ms;

 // 闹铃节奏：100ms 块播，每块之间检查 stop_requested_
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

/**
 * @brief 播放任务主循环
 * 用 FreeRTOS 任务循环轮询 pending_track_ / pending_bell_hour_。
 * 报时/闹钟时播放对应音频，完成后交还 AI 语音。
 */
    // 播放任务入口
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
 // 播放单曲 - 在此管理静音
            self->is_playing_ = true;
            int track = self->pending_track_;
            self->pending_track_ = 0;
            if (!self->disable_ducking_) {
                Application::GetInstance().GetAudioService().SetOutputMuted(true);
            }
            self->DecodeSingleFile(track);
            self->is_playing_ = false;
            if (!self->disable_ducking_) {
                Application::GetInstance().GetAudioService().SetOutputMuted(false);
            }
        }

        if (self->pending_bell_hour_ > 0) {
 // 播放报时钟声：短 PCM 铃声（每次约 0.5 秒）
            self->is_playing_ = true;
            int hour = self->pending_bell_hour_;
            self->pending_bell_hour_ = 0;
            Application::GetInstance().GetAudioService().SetOutputMuted(true);
            for (int i = 0; i < hour; i++) {
                if (self->stop_requested_) break;
                self->bell_player_.PlayBellSoundSync();
 // 两次敲击间隔约 1 秒，模拟自然钟声
                if (i < hour - 1 && !self->stop_requested_) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            self->is_playing_ = false;
            Application::GetInstance().GetAudioService().SetOutputMuted(false);
        }
    }
}

/**
 * @brief 播放指定文件夹/曲目（兼容 DFPlayer 接口）
 * @param folder 文件夹号：1=报时铃声，2=音乐
 * @param track 曲目号（音乐自动钳位到 1~12）
 */
void Mp3Player::PlayTrack(uint8_t folder, uint8_t track) {
    if (folder == 1) {
        // 播放铃声
        PlayBell(track);
    } else if (folder == 2) {
        // 播放音乐
        if (track < 1) track = 1;
        if (track > 12) track = 12;
        PlayIndex(track);
    }
}

/**
 * @brief 按编号播放音乐（异步，经播放任务执行）
 * @param index 音乐编号（自动钳位 >=1）
 * 停止当前播放→重置状态→置 pending_track_→确保播放任务已创建。
 */
void Mp3Player::PlayIndex(uint16_t index) {
    if (index < 1) index = 1;

    if (!assets_) {
        ESP_LOGE(TAG, "Mp3Player not initialized (call Init first)");
        return;
    }

    // 停止当前播放
    Stop();

    // 重置 stop_requested_ 并设置播放目标
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;

    // 确保播放任务已创建
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

/**
 * @brief 停止当前播放
 * 置 stop_requested_ 并清空待播队列，重置解码器。
 * 注意：不直接置 is_playing_=false，由播放任务自行退出。
 */
void Mp3Player::Stop() {
    stop_requested_ = true;
 // 不要在这里设置 is_playing_=false，应由 PlayOpusTask 自行设置
 // 调用 PlayOpus() 可以可靠地等待播放任务退出
    pending_track_ = 0;
    pending_bell_hour_ = 0;

    if (mp3_dec_handle_) {
        esp_mp3_dec_reset(mp3_dec_handle_);
    }
    
 // 等待音频队列排空，用 WaitForPlaybackQueueEmpty() 后再返回
    Application::GetInstance().GetAudioService().ResetDecoder();
}

/**
 * @brief 暂停播放（置停止标志并清 is_playing_）
 */
void Mp3Player::Pause() {
    stop_requested_ = true;
    is_playing_ = false;
}

/**
 * @brief 重置播放状态，准备下一次播放
 * 清 stop_requested_、ducking 状态、is_playing_、待播队列。
 */
void Mp3Player::ResetForNextPlay() {
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    is_playing_ = false;
    pending_track_ = 0;
    pending_bell_hour_ = 0;
}

/**
 * @brief 恢复播放（不支持，需重新调用 PlayIndex）
 */
void Mp3Player::Resume() {
    ESP_LOGW(TAG, "Resume not supported, call PlayIndex again");
}

/**
 * @brief 设置音量（实际由 AudioService 统一处理，此处仅记录日志）
 * @param vol 音量值 0~100
 */
void Mp3Player::SetVolume(uint8_t vol) {
    ESP_LOGI(TAG, "Volume request: %d (handled by AudioService)", vol);
}

// ---- 音量/切歌兼容接口（空实现，音量由 AudioService 处理）----
void Mp3Player::VolumeUp() { SetVolume(20); }
void Mp3Player::VolumeDown() { SetVolume(10); }
void Mp3Player::Next() {}
void Mp3Player::Prev() {}

/**
 * @brief 播放报时钟声（异步，经播放任务执行）
 * @param hour 报时点数 1~12（自动钳位）
 * 停止当前播放→置 pending_bell_hour_→确保播放任务已创建；
 * 任务内每点敲一次铃，间隔约 1 秒模拟自然钟声。
 */
void Mp3Player::PlayBell(int hour) {
    if (hour < 1) hour = 1;
    if (hour > 12) hour = 12;

    if (!assets_) return;

    // 停止当前播放，为新报时清场
    Stop();

    // 记录报时点数，异步任务中读取
    pending_bell_hour_ = hour;

    // 确保播放任务已创建
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

/**
 * @brief 播放背景音乐（等价于 PlayIndex）
 * @param index 音乐编号（>=1）
 */
void Mp3Player::PlayBgMusic(int index) {
    if (index < 1) index = 1;
    PlayIndex(index);
}

/**
 * @brief 将 MP3 资源一次性解码到 PSRAM 缓冲
 * @param index MP3 编号（如 15 → 0015.mp3）
 * @param out_buf 输出 PCM 缓冲（PSRAM 分配，调用方负责释放）
 * @param out_samples 输出采样数
 * @param out_samplerate 输出采样率
 * @return 0=成功；-1=失败
 * 跳过 ID3v2 标签逐帧解码，PCM 上限 2M 采样（4MB）。
 */
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

 // 内存分配：MP3 解码后 PCM 数据约 2.75 倍，4 倍保证安全
    size_t max_samples = mp3_size * 4;
    if (max_samples < 8192) max_samples = 8192;
    if (max_samples > 2 * 1024 * 1024) max_samples = 2 * 1024 * 1024;  // 上限 2M 采样（4MB）
    int16_t* buf = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu samples in PSRAM", (unsigned long)max_samples);
        return -1;
    }

    // 打开解码器
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

    // 跳过 ID3v2 标签
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

 // 逐帧解码扫描
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
 // 解码器已缓冲全部输入，无法继续，停止
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

    // 关闭解码器
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
    ESP_LOGI(TAG, "Decoded %s to PCM: %u samples, %d Hz", filename, (unsigned)written, sample_rate);
    return 0;
}

/**
 * @brief 流式解码 MP3：逐帧回调输出 PCM，零大 buffer，适合长音频（如 0009.mp3）
 * @param index MP3 编号
 * @param callback 每帧回调 (pcm, samples, src_sr, user_data)
 * @param user_data 透传给回调的参数
 * @return 0=成功；-1=失败
 */
int Mp3Player::DecodeStreaming(int index,
    std::function<void(const int16_t* pcm, size_t samples, int src_sr, void* user_data)> callback,
    void* user_data) {
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

    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }
    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return -1;
    }

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
    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;

    while (remaining > 0 && !stop_requested_) {
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
            if (frame.decoded_size > 0) {
                size_t n = frame.decoded_size / sizeof(int16_t);
                int sr = dec_info.sample_rate ? dec_info.sample_rate : 16000;
                callback((const int16_t*)frame.buffer, n, sr, user_data);
            }
            size_t consumed = raw.consumed;
            if (consumed == 0) break;
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

    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    return 0;
}

// ============================================
// LDR 光敏电阻读取 (ADC oneshot 模式)
// ============================================
LdrSensor::LdrSensor(gpio_num_t adc_pin, adc_unit_t unit, adc_channel_t chan, int threshold)
    : adc_pin_(adc_pin), adc_handle_(nullptr), adc_chan_(chan), threshold_(threshold) {

 // ---- ADC oneshot 初始化 ----
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

/**
 * @brief 析构 LDR 传感器，释放 ADC oneshot 单元
 */
LdrSensor::~LdrSensor() {
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }
}

/**
 * @brief 读取光敏电阻原始 ADC 值
 * @return 0-4095（12 位），0=最暗，4095=最亮
 */
int LdrSensor::ReadRaw() {
    int raw = 0;
    if (adc_handle_) {
        adc_oneshot_read(adc_handle_, adc_chan_, &raw);
    }
    return raw;  // 0-4095 (12-bit), 0=暗值, 4095=亮值
}

/**
 * @brief 判断当前是否为黑暗，结合阈值与实时采样值
 * @return true=黑暗, false=明亮
 */
bool LdrSensor::IsDark() { return ReadRaw() < threshold_; }
/**
 * @brief 设置黑暗判定阈值
 * @param threshold ADC 原始读数阈值，低于此值判定为暗
 */
void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }


// ============================================
// BellSoundPlayer - 布谷鸟叫声播放器，通过 AI 音频系统输出
// ============================================
void BellSoundPlayer::PlayCuckooSoundSync() {
    // 不需要 SetOutputMuted，也不需要 vTaskDelay
    auto& app = Application::GetInstance();
    if (app.GetAudioService().IsBgAudioActive()) {
        // 音乐播放中：将布谷鸟叫声混入背景音频（不打断）
        for (int repeat = 0; repeat < 2; repeat++) {
            app.GetAudioService().MixIntoBackgroundAudio(
                cuckoo_wake_sound, CUCKOO_WAKE_SOUND_NUM_SAMPLES, 0.9f);
            if (repeat < 1) vTaskDelay(pdMS_TO_TICKS(50));
        }
    } else {
        app.GetAudioService().OutputRawPcm(
            cuckoo_wake_sound,
            CUCKOO_WAKE_SOUND_NUM_SAMPLES,
            CUCKOO_WAKE_SOUND_SAMPLE_RATE
        );
    }
}

/**
 * @brief 同步播放报时钟声（直接写 I2S）
 * 经 OutputRawPcm 输出，与 AudioOutputTask 共用 data_if_mutex_ 同步。
 */
void BellSoundPlayer::PlayBellSoundSync() {
    // 直接写 I2S 是直接输出，使用 data_if_mutex_ 与 AudioOutputTask 同步
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_bell_sound,
        CUCKOO_BELL_SOUND_NUM_SAMPLES,
        CUCKOO_BELL_SOUND_SAMPLE_RATE
    );
}

/**
 * @brief 异步播放布谷鸟叫声（创建临时任务，播完自删）
 */
void BellSoundPlayer::PlayCuckooSoundAsync() {
    xTaskCreate([](void* arg) {
        auto* self = static_cast<BellSoundPlayer*>(arg);
        self->PlayCuckooSoundSync();
        vTaskDelete(NULL);
    }, "cuckoo_async", 2048, this, 5, NULL);
}

// ============================================
// CuckooStateMachine - 布谷鸟状态机
// ============================================
CuckooStateMachine::CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                                        Motor* violin_motor, Servo* violin, Servo* dog,
                                        Motor* water_bird, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
      violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),  // 小提琴电机 (电机 B M2, GPIO18/45)
      water_bird_(water_bird),
      mp3_(mp3), bell_player_(bell_player), ldr_(ldr),
      motor_power_pin_(GPIO_NUM_NC),
      current_performance_(kPerformanceNone), current_phase_(kPhaseIdle),
      call_count_(0), total_calls_(0), is_running_(false),
      last_hour_(-1), last_half_hour_(-1), show_music_index_(0),
      dog_outro_running_(false), dog_intro_running_(false), kids_active_(false),
      current_hour_(12), current_min_(0), current_sec_(0), time_set_(false),
      is_dark_(false) {
    last_idle_exit_us_ = 0;
    prev_device_state_ = -1;
    // 电机电源 P-MOSFET 控制 (GPIO LOW=ON, HIGH=OFF)
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);
 // LED（GPIO1，S8050 B/C 极，5V）
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_A_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    MotorPowerOff();  // 默认断电，100K 下拉保证 5V
  }

