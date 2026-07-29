int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        ESP_LOGW(TAG, "PlayOpus: music already playing, refusing (call stop_music first)");
        return -1;
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
    stop_requested_ = false;  // 锟斤拷锟?Stop() 锟斤拷锟矫的憋拷志
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;

    // Clear stale bg audio from previous session to prevent startup noise burst
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

    xTaskCreatePinnedToCore(PlayOpusTask, "opus_http", 1024 * 24, ctx, 3, NULL, 1);  // 24KB stack for OGG/Opus decode
    return 0;
}

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

 // 锟斤拷锟斤拷使锟矫憋拷锟斤拷锟斤拷频锟姐，锟斤拷Ducking锟斤拷锟斤拷AI锟斤拷锟斤拷

    // 锟斤拷锟斤拷URL
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
 // 锟饺筹拷锟皆碉拷锟绞拷锟斤拷锟絀P锟斤拷锟劫筹拷锟斤拷DNS锟斤拷锟斤拷
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
    
 // 锟斤拷锟斤拷锟斤拷connect+5锟诫超时锟斤拷SO_SNDTIMEO锟斤拷lwip锟较诧拷锟斤拷效锟斤拷
 // 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷止goto serial_fallback锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷始锟斤拷
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
        fcntl(sock, F_SETFL, sock_flags);  // restore blocking mode
    }
    
    if (false) {  // gate for serial fallback
serial_fallback:
 // 锟斤拷锟节伙拷锟斤拷路锟斤拷锟斤拷通锟斤拷goto锟斤拷锟斤，sock锟窖关闭ｏ拷
        
 // === Serial fallback ===
        printf("\x01MUSIC_REQ\x02%s\x03\n", url);
        fflush(stdout);
        
 // 锟斤拷锟酵Ｖ癸拷锟街撅拷锟斤拷锟絇layOpus锟叫碉拷Stop()锟斤拷锟矫ｏ拷锟斤拷锟斤拷fread循锟斤拷锟斤拷锟斤拷
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
        
 // 锟斤拷UART0 RX锟斤拷取Opus锟斤拷锟捷ｏ拷锟斤拷始锟接筹拷锟矫达拷锟斤拷转锟斤拷锟斤拷锟斤拷取锟斤拷锟斤拷
        vTaskDelay(pdMS_TO_TICKS(3000));  // Wait 3s for serial_relay to fetch data
        
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
 // 锟斤拷止AFE锟斤拷源锟斤拷锟斤拷锟截憋拷锟斤拷朔锟?
                uint64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_refresh_ms > 8000) {  // every 8s
 // HACK: Prevent audio watchdog timeout during long downloads
                app.GetAudioService().RefreshInputTimestamp();

                    last_refresh_ms = now_ms;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            uint64_t now = esp_timer_get_time() / 1000;
            if (stream_started && (now - last_data_ms > 5000)) break;  // 5s idle
            if (!stream_started && (now - serial_start > 15000)) break;  // 15s startup
            if (now - serial_start > 120000) break;  // 2min total
            if (app.GetDeviceState() == kDeviceStateConnecting) break;  // 锟斤拷锟窖词达拷锟斤拷 stop
 // 500ms锟斤拷锟剿拷丶锟解，锟斤拷止锟斤拷锟斤拷锟斤拷锟窖碉拷锟铰硷拷停止
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
    
    // 锟斤拷锟斤拷HTTP GET锟斤拷锟斤拷
    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    send(sock, req, strlen(req), 0);
    
    // 锟斤拷取HTTP锟斤拷应头
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

 // === Detect format: /opus uses OGG demuxer + main audio pipeline ===
 // === /pcm uses raw bytes pushed to background ring buffer ===
    bool use_opus = (strstr(path, "/opus") != nullptr);
    
    if (use_opus) {
        // ============ Opus path: OGG demux + PushPacketToDecodeQueue ============
 // 锟斤拷AI锟斤拷锟斤拷Opus锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷同一锟斤拷锟斤拷锟脚讹拷锟叫癸拷锟斤拷
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
        
 // 锟斤拷锟街诧拷锟斤拷时锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷65%锟斤拷锟斤拷锟斤拷AEC锟截采革拷锟斤拷
        auto* codec = Board::GetInstance().GetAudioCodec();
        int old_vol = codec ? codec->output_volume() : 90;
        int music_vol = old_vol * 65 / 100;
        if (music_vol < 10) music_vol = 10;
        if (codec) codec->SetOutputVolume(music_vol);
        ESP_LOGI(TAG, "PlayOpus: volume %d -> %d for AEC", old_vol, music_vol);
        
        bool is_ducking = false;  // true = AI speaking, don't push music
        
        while (self->is_playing_ && !self->stop_requested_) {
            int read = recv(sock, buf, CHUNK, 0);
            if (read <= 0) {
                ESP_LOGW(TAG, "PlayOpus: recv returned %d errno=%d", read, (read < 0) ? errno : 0);
                break;
            }
            total_dl += read;

 // Ducking: AI说锟斤拷时锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟捷碉拷锟斤拷锟狡革拷锟斤拷锟斤拷锟斤拷
 // 锟斤拷锟斤拷speaking状态时ResetDecoder锟斤拷锟斤拷锟斤拷私锟斤拷锟斤拷锟斤拷
 // 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟捷伙拷使锟斤拷锟斤拷锟斤拷前锟斤拷锟斤拷一小锟轿ｏ拷5-15锟斤拷锟紸I锟截革拷锟叫硷拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷
 // 锟斤拷一锟斤拷3-5锟斤拷锟接的革拷锟斤拷5-15锟斤拷锟紸I锟截革拷锟叫硷拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷
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
                demuxer->Process(buf, read);  // keep decoder state, skip output
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
        
        // 锟街革拷锟斤拷锟斤拷
        if (codec) codec->SetOutputVolume(old_vol);
        ESP_LOGI(TAG, "PlayOpus: volume restored to %d", old_vol);
        
 // 锟饺达拷锟斤拷锟斤拷锟斤拷锟斤拷趴锟?
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
    
 // ============ PCM path (legacy): raw s16le PushBackgroundAudio ============
    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    int64_t t0 = esp_timer_get_time(), last_refresh = t0;
    size_t total_dl = 0;
    bool ai_speaking = false;
    int prev_ai_level = 0;   // 0=idle, 1=listening, 2=speaking/connecting

    ESP_LOGI(TAG, "PlayOpus: downloading raw PCM...");

 // === Pre-buffer phase: fill ring buffer before enabling drain ===
    int64_t prebuf_start = esp_timer_get_time();
    while (self->is_playing_ && !self->stop_requested_) {
        int read = recv(sock, buf, CHUNK, 0);
        if (read <= 0) break;
        total_dl += read;
        app.GetAudioService().PushBackgroundAudio(
            reinterpret_cast<int16_t*>(buf), read / sizeof(int16_t), 16000);

        size_t fill = app.GetAudioService().GetBgAudioFillLevel();
        if (fill >= 64000) {
            app.GetAudioService().SetBackgroundAudioGain(0.3f);  // activate at low volume
            app.GetAudioService().EnableBgAudioDrain(true);
            app.GetAudioService().SetBackgroundAudioGain(1.0f);  // fade-in via ~300ms ramp
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

 // === Main download push loop ===
    int recv_errors = 0;
    while (self->is_playing_ && !self->stop_requested_) {
        auto state = app.GetDeviceState();
        // 3-level ducking: idle=100%, listening=70%, speaking/connecting=50%
        int ai_level = 0;
        if (state == kDeviceStateSpeaking || state == kDeviceStateConnecting) {
            ai_level = 2;
        } else if (state == kDeviceStateListening) {
            ai_level = 1;
        }
        // Track ai_speaking for WiFi throttle logic (level 2 only)
        bool ai_now = (ai_level == 2);
        if (ai_now != ai_speaking) {
            ai_speaking = ai_now;
        }
        if (ai_level != prev_ai_level) {
            prev_ai_level = ai_level;
            float gain = (ai_level == 2) ? 0.5f : (ai_level == 1) ? 0.6f : 1.0f;
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

 // When AI is speaking, pause TCP download to free WiFi airtime for
 // UDP audio packets (prevents WiFi buffer starvation TTS stutter).
 // Only pause if buffer sufficient to ride through typical AI reply.
        if (ai_speaking && app.GetAudioService().GetBgAudioFillLevel() > 64000) {
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
 // Server closed connection gracefully
            ESP_LOGI(TAG, "PlayOpus: server closed connection");
            break;
        } else {
 // read < 0: timeout or transient error
            recv_errors++;
            if (recv_errors > 5) {
                ESP_LOGE(TAG, "PlayOpus: recv failed %d times, giving up (errno=%d)", recv_errors, errno);
                break;
            }
 // Wait 1s before retry WiFi may be reconnecting after AI conversation
            ESP_LOGW(TAG, "PlayOpus: recv error %d/%d (errno=%d), retrying...", recv_errors, 5, errno);
            for (int w = 0; w < 10 && self->is_playing_ && !self->stop_requested_; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
 // Refresh power save in case it was changed by channel close
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            continue;
        }

        while (app.GetAudioService().GetBgAudioFillLevel() > 80000 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_refresh > 8000000) {
 // Diagnostic: log buffer fill + download rate every 8s
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
        // 鐩存帴娓呯┖锛屼笉绛?ring buffer 鎺掔┖
        // 鍘熸潵 while 绛夊緟鎺掔┖浼氬鑷?MusicDanceTick 缁х画璺?~5 绉掞紙79K 鏍锋湰 梅 16kHz锛?
        app.GetAudioService().ClearBackgroundAudio();
    }
    close(sock);
    self->is_playing_ = false;
    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "PlayOpus: done %dms, dl=%dKB", (int)total_ms, (int)(total_dl / 1024));

    // 锟借备锟斤拷锟斤拷时锟斤拷锟斤拷锟斤拷锟窖词硷拷猓凤拷锟斤拷锟剿凤拷锟斤拷锟斤拷通锟斤拷锟斤拷锟?
    // 锟斤拷时锟戒背锟斤拷锟斤拷频锟斤拷锟脚猴拷锟斤拷频锟斤拷锟斤拷路锟斤拷锟斤拷锟斤拷锟斤拷停止
    // 锟斤拷esp_codec_dev_read锟斤拷锟截癸拷锟斤拷/锟斤拷锟斤拷锟捷碉拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷
    // 锟斤拷锟窖词硷拷锟斤拷锟斤拷锟斤拷锟斤拷锟?锟斤拷锟斤拷锟斤拷"锟斤拷实锟斤拷锟斤拷锟斤拷
    if (app.GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "PlayOpus: restarting wake word detection after music");
        app.GetAudioService().EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        app.GetAudioService().EnableWakeWordDetection(true);
        // Fix(2026-07-19): idle transition during music skips threshold restore
        // (IsBgAudioActive guard), leaving 0.30 stuck. Restore sensitive 0.02 here.
        app.GetAudioService().SetWakeWordThreshold(0.02f);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 锟斤拷锟斤拷锟斤拷锟斤拷ringtone
 * @param volume 锟斤拷锟斤拷系锟斤拷 0.0~1.0锟斤拷锟斤拷锟藉开头锟斤拷15%锟斤拷强锟斤拷100%
 * 锟斤拷锟斤拷A5(880Hz)/C#6(1100Hz)锟斤拷锟斤拷锟絇CM锟斤拷锟斤拷锟斤拷锟斤拷每锟斤拷2锟斤拷
 */
void Mp3Player::PlayAlarmRing(float volume) {
    auto& app = Application::GetInstance();
    const int sample_rate = 16000;
    const int chunk_ms = 100;  // 100ms chunks for responsive stop
    const int total_ms = 2000;
    const int chunk_samples = sample_rate * chunk_ms / 1000;
    const int total_chunks = total_ms / chunk_ms;

    const int freq_a = 880;   // A5
    const int freq_b = 1100;  // C#6
    const int base_amplitude = 8000;
    const int beep_on_ms = 80;
    const int beep_off_ms = 80;
    const int cycle_ms = beep_on_ms + beep_off_ms;

 // 锟斤拷锟缴诧拷锟斤拷100ms锟介播锟脚ｏ拷每锟斤拷之锟斤拷锟斤拷stop_requested_
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
 * @brief 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷循锟斤拷
 * 锟斤拷FreeRTOS锟斤拷锟斤拷锟斤拷锟斤拷询 pending_track_ / pending_bell_hour_锟斤拷
 * 锟斤拷锟斤拷锟斤拷时锟斤拷锟斤拷AI锟斤拷锟斤拷锟斤拷锟斤拷哦锟接︼拷锟狡碉拷锟斤拷锟斤拷锟斤拷指锟紸I锟斤拷锟?
 */
    // 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟?
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
 // play single track - manage mute here
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
 // play bell chime using short PCM bell sound (~0.5s each)
            self->is_playing_ = true;
            int hour = self->pending_bell_hour_;
            self->pending_bell_hour_ = 0;
            Application::GetInstance().GetAudioService().SetOutputMuted(true);
            for (int i = 0; i < hour; i++) {
                if (self->stop_requested_) break;
                self->bell_player_.PlayBellSoundSync();
 // pause ~1s between strikes for natural clock sound
                if (i < hour - 1 && !self->stop_requested_) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            self->is_playing_ = false;
            Application::GetInstance().GetAudioService().SetOutputMuted(false);
        }
    }
}

void Mp3Player::PlayTrack(uint8_t folder, uint8_t track) {
    if (folder == 1) {
        // 锟斤拷锟斤拷
        PlayBell(track);
    } else if (folder == 2) {
        // 锟斤拷锟斤拷
        if (track < 1) track = 1;
        if (track > 12) track = 12;
        PlayIndex(track);
    }
}

void Mp3Player::PlayIndex(uint16_t index) {
    if (index < 1) index = 1;

    if (!assets_) {
        ESP_LOGE(TAG, "Mp3Player not initialized (call Init first)");
        return;
    }

    // 停止锟斤拷前锟斤拷锟斤拷
    Stop();

    // 锟斤拷锟?stop_requested_ 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷目
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;

    // 确锟斤拷锟斤拷锟斤拷锟窖达拷锟斤拷
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

void Mp3Player::Stop() {
    stop_requested_ = true;
 // 锟斤拷要锟节达拷锟斤拷锟斤拷is_playing_=false 锟斤拷 锟斤拷PlayOpusTask锟斤拷锟斤拷锟皆硷拷锟斤拷
 // 锟斤拷锟斤拷PlayOpus()锟斤拷锟皆可匡拷锟截等达拷锟斤拷锟斤拷锟斤拷锟剿筹拷
    pending_track_ = 0;
    pending_bell_hour_ = 0;

    if (mp3_dec_handle_) {
        esp_mp3_dec_reset(mp3_dec_handle_);
    }
    
 // 锟斤拷锟斤拷锟斤拷频锟斤拷锟叫ｏ拷锟矫撅拷锟斤拷锟斤拷锟絎aitForPlaybackQueueEmpty()锟斤拷锟劫凤拷锟斤拷
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void Mp3Player::Pause() {
    stop_requested_ = true;
    is_playing_ = false;
}

void Mp3Player::ResetForNextPlay() {
    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    is_playing_ = false;
    pending_track_ = 0;
    pending_bell_hour_ = 0;
}

void Mp3Player::Resume() {
    ESP_LOGW(TAG, "Resume not supported, call PlayIndex again");
}

void Mp3Player::SetVolume(uint8_t vol) {
    ESP_LOGI(TAG, "Volume request: %d (handled by AudioService)", vol);
}

void Mp3Player::VolumeUp() { SetVolume(20); }
void Mp3Player::VolumeDown() { SetVolume(10); }
void Mp3Player::Next() {}
void Mp3Player::Prev() {}

void Mp3Player::PlayBell(int hour) {
    if (hour < 1) hour = 1;
    if (hour > 12) hour = 12;

    if (!assets_) return;

    // 停止锟斤拷前锟斤拷锟斤拷
    Stop();

    // 璁剧疆锟斤拷锟斤拷閲嶅娆℃暟
    pending_bell_hour_ = hour;

    // 确锟斤拷锟斤拷锟斤拷锟窖达拷锟斤拷
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

void Mp3Player::PlayBgMusic(int index) {
    if (index < 1) index = 1;
    PlayIndex(index);
}

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

 // 锟斤拷锟斤拷锟斤拷洌篗P3锟筋坏锟斤拷锟絇CM锟斤拷锟斤拷约2.75锟斤拷锟斤拷4锟斤拷锟角筹拷锟斤拷全
    size_t max_samples = mp3_size * 4;
    if (max_samples < 8192) max_samples = 8192;
    if (max_samples > 2 * 1024 * 1024) max_samples = 2 * 1024 * 1024;  // cap at 2M samples (4MB)
    int16_t* buf = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu samples in PSRAM", (unsigned long)max_samples);
        return -1;
    }

    // 锟津开斤拷锟斤拷锟斤拷
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

    // 锟斤拷锟斤拷 ID3v2 锟斤拷签
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

 // 锟斤拷锟轿斤拷锟斤拷扫锟斤拷
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
 // Decoder buffered all input; no progress possible, stop
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

    // 锟截闭斤拷锟斤拷锟斤拷
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

// ============================================
// LDR 锟斤拷锟斤拷锟斤拷锟借传锟斤拷锟斤拷 (ADC oneshot模式)
// ============================================
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
    return raw;  // 0-4095 (12-bit), 锟斤拷=锟斤拷值, 锟斤拷=锟斤拷值
}

/**
 * @brief 锟叫断碉拷前锟角凤拷为锟节帮拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟街碉拷锟?
 * @return true=锟节帮拷, false=锟斤拷锟斤拷
 */
bool LdrSensor::IsDark() { return ReadRaw() < threshold_; }
void LdrSensor::SetThreshold(int threshold) { threshold_ = threshold; }


// ============================================
// BellSoundPlayer - 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟酵拷锟紸I锟斤拷频系统锟斤拷锟斤拷锟?
// ============================================
void BellSoundPlayer::PlayCuckooSoundSync() {
    // 锟斤拷锟斤拷要 SetOutputMuted锟斤拷也锟斤拷锟斤拷要 vTaskDelay
    auto& app = Application::GetInstance();
    if (app.GetAudioService().IsBgAudioActive()) {
        // Music playing: mix cuckoo sound into bg audio (no interruption)
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

void BellSoundPlayer::PlayBellSoundSync() {
    // 直锟斤拷写 I2S 锟斤拷锟斤拷直锟斤拷锟斤拷锟疥，使锟斤拷 data_if_mutex_ 锟斤拷 AudioOutputTask 锟斤拷锟斤拷
    auto& app = Application::GetInstance();
    app.GetAudioService().OutputRawPcm(
        cuckoo_bell_sound,
        CUCKOO_BELL_SOUND_NUM_SAMPLES,
        CUCKOO_BELL_SOUND_SAMPLE_RATE
    );
}

void BellSoundPlayer::PlayCuckooSoundAsync() {
    xTaskCreate([](void* arg) {
        auto* self = static_cast<BellSoundPlayer*>(arg);
        self->PlayCuckooSoundSync();
        vTaskDelete(NULL);
    }, "cuckoo_async", 2048, this, 5, NULL);
}

// ============================================
// CuckooStateMachine - 锟斤拷锟斤拷锟斤拷锟斤拷状态锟斤拷
// ============================================
CuckooStateMachine::CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                                        Motor* violin_motor, Servo* violin, Servo* dog,
                                        Motor* water_bird, Mp3Player* mp3,
                                        BellSoundPlayer* bell_player,
                                        LdrSensor* ldr)
    : m1_(m1), m2_(m2), m3_(m3), m4_(m4),
      violin_motor_(violin_motor), violin_servo_(violin), dog_servo_(dog),  // 小锟斤拷锟劫碉拷锟?(锟斤拷B M2, GPIO18/45)
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
    // 锟斤拷锟斤拷锟皆?P-MOSFET 锟斤拷锟斤拷 (GPIO LOW=ON, HIGH=OFF)
    motor_power_pin_ = (gpio_num_t)POWER_MOTOR_GPIO;
    gpio_config_t motor_pwr_cfg = {
        .pin_bit_mask = (1ULL << motor_power_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&motor_pwr_cfg);
 // LED (GPIO1KS8050 B, C, 5V)
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_A_GPIO) | (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_A_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    MotorPowerOff();  // 默锟较断电，100K 锟斤拷锟斤拷锟斤拷锟斤拷 5V
  }

