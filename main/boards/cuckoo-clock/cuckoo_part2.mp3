// ===== Part 2: Mp3Player 类 (L284-1924) =====
    assets_ = assets;
    ESP_LOGI(TAG, "Mp3Player initialized (async task-based software decode)");
}




void Mp3Player::UpdateDuckingState() {
    auto& app = Application::GetInstance();
    auto dev_state = app.GetDeviceState();
    if (dev_state == kDeviceStateSpeaking && ducking_gain_ >= 1.0f) {
        ducking_gain_ = 1.0f;
        ducking_start_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Ducking: AI speaking, fading out");
    }
    if (ducking_gain_ < 1.0f) {
        if (dev_state != kDeviceStateSpeaking) {
            ducking_gain_ = 1.0f; ducking_start_us_ = 0;
        } else {
            int64_t elapsed = esp_timer_get_time() - ducking_start_us_;
            ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
            if (ducking_gain_ < 0.2f) {
                ducking_gain_ = 0.2f;
                ESP_LOGI(TAG, "Ducking: fade complete at 20%%");
            }
        }
    }
}

/**



 */
void Mp3Player::LoadDogBark(const int16_t* pcm, size_t num_samples) {
    while (bark_active_) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bark_pcm_ = std::vector<int16_t>(pcm, pcm + num_samples);
    bark_total_ = num_samples;
    bark_offset_ = 0;
    bark_active_ = true;
    ESP_LOGI(TAG, "DogBark: loaded %u samples, mixing into music output", (unsigned)num_samples);
}

void Mp3Player::ApplyDuckingGain(int16_t* pcm, size_t num_samples) {
    float g = ducking_gain_;
    if (g >= 1.0f) return;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t s = (int32_t)(pcm[i] * g);
        if (s > 30000) s = 30000;
        if (s < -30000) s = -30000;
        pcm[i] = (int16_t)s;
    }
}

/**
 * @brief ���벢���ŵ�������MP3�ļ�
  * @param index MP3 文件编号 (0001~0012 歌曲, 0013 报时铃声, 0015 琳达, 0016 花园)
 * @return 0=�����ֹͣ, 1=��������
    // 跳过 ID3v2 标签
 * - ���л��������bark_active_�Զ����ӹ�����
 * - Ducking��AI˵��ʱ����20%����
 */
int Mp3Player::DecodeSingleFile(int index) {
    if (!assets_) {
        ESP_LOGE(TAG, "Assets not initialized");
        return 0;
    }

    char filename[16];
    snprintf(filename, sizeof(filename), "%04d.mp3", index);

    // 从 assets 分区读取 MP3 文件数据
    void* mp3_data = nullptr;
    size_t mp3_size = 0;
    if (!assets_->GetAssetData(filename, mp3_data, mp3_size)) {
        ESP_LOGE(TAG, "Failed to get asset: %s", filename);
        return 0;
    }

    if (!mp3_data || mp3_size == 0) {
        ESP_LOGE(TAG, "Empty asset: %s", filename);
        return 0;
    }

    ESP_LOGI(TAG, "Playing %s (%lu bytes)", filename, (unsigned long)mp3_size);

    // 跳过 ID3v2 标签
    uint8_t* mp3_start = (uint8_t*)mp3_data;
    size_t mp3_data_size = mp3_size;
    if (mp3_size > 10 && memcmp(mp3_start, "ID3", 3) == 0) {
        uint32_t id3_size = ((mp3_start[6] & 0x7F) << 21) | ((mp3_start[7] & 0x7F) << 14)
                          | ((mp3_start[8] & 0x7F) << 7)  |  (mp3_start[9] & 0x7F);
        size_t skip = 10 + id3_size;
        if (skip < mp3_size - 1024) {  // ȷ�����������㹻����
            mp3_start += skip;
            mp3_data_size -= skip;
            ESP_LOGI(TAG, "Skipped ID3v2 tag: %lu bytes", (unsigned long)skip);
        }
    }


    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }

    esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &mp3_dec_handle_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return 0;
    }


    esp_audio_dec_info_t dec_info = {};
    bool info_ready = false;
    int sample_rate = 22050;
    int channels = 1;


    uint8_t* input_ptr = mp3_start;
    size_t remaining = mp3_data_size;
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 50;

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

            if (!info_ready && frame.decoded_size > 0) {
                sample_rate = dec_info.sample_rate ? dec_info.sample_rate : 22050;
                channels = dec_info.channel ? dec_info.channel : 1;
                ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %.1f kbps",
                         sample_rate, channels, dec_info.bitrate / 1000.0f);
                info_ready = true;
            }


            if (frame.decoded_size > 0 && !stop_requested_) {
                int16_t* pcm_data = (int16_t*)frame.buffer;
                size_t num_samples = frame.decoded_size / sizeof(int16_t);

                auto& app = Application::GetInstance();

 // ---- Smooth ducking: fade music out when AI starts speaking ----
                // Use a state machine: 0=idle, 1=fading, 2=ducked, 3=restoring
                static int duck_state = 0;  // 0=idle, 1=fading, 2=ducked, 3=restoring
                static int64_t duck_transition_us = 0;
                bool ai_speaking = (app.GetDeviceState() == kDeviceStateSpeaking);

                if (disable_ducking_) {

                    ducking_gain_ = 1.0f;
                } else if (ai_speaking && duck_state == 0) {

                    duck_state = 1;
                    duck_transition_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "DecodeSingleFile: AI speaking, fading out");
                }

                if (duck_state == 1) {

                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ <= 0.2f) {
                        ducking_gain_ = 0.2f;
                        duck_state = 2;
                    }
                    if (!ai_speaking) {

                        duck_state = 3;
                        duck_transition_us = esp_timer_get_time();
                    }
                } else if (duck_state == 2) {
                    ducking_gain_ = 0.2f;
                    if (!ai_speaking) {
                        duck_state = 3;
                        duck_transition_us = esp_timer_get_time();
                    }
                } else if (duck_state == 3) {

                    int64_t elapsed = esp_timer_get_time() - duck_transition_us;
                    ducking_gain_ = 0.2f + 0.8f * (float)elapsed / 400000.0f;
                    if (ducking_gain_ >= 1.0f) {
                        ducking_gain_ = 1.0f;
                        duck_state = 0;
                        ESP_LOGI(TAG, "DecodeSingleFile: false trigger, restored");
                    }
                    if (ai_speaking) {

                        duck_state = 1;
                        duck_transition_us = esp_timer_get_time();
                    }
                }

 // Apply gain + clip
                float gain = ducking_gain_;
                for (size_t i = 0; i < num_samples; i++) {
                    int32_t s = (int32_t)(pcm_data[i] * gain);
                    if (s > 30000) s = 30000;
                    if (s < -30000) s = -30000;
                    pcm_data[i] = (int16_t)s;
                }


                if (bark_active_) {
                    size_t to_mix = num_samples;
                    if (bark_offset_ + to_mix > bark_total_) {
                        to_mix = bark_total_ - bark_offset_;
                    }
                    for (size_t i = 0; i < to_mix; i++) {
                        int32_t mixed = (int32_t)pcm_data[i] + (int32_t)bark_pcm_[bark_offset_ + i];
                        if (mixed > 32767) mixed = 32767;
                        else if (mixed < -32768) mixed = -32768;
                        pcm_data[i] = (int16_t)mixed;
                    }
                    bark_offset_ += to_mix;
                    if (bark_offset_ >= bark_total_) {
                        bark_active_ = false;
                        ESP_LOGI(TAG, "DogBark: mix done (%u samples)", (unsigned)bark_total_);
                    }
                }

                app.GetAudioService().OutputRawPcm(pcm_data, num_samples, sample_rate);


                int play_ms = (int)(num_samples * 1000 / sample_rate / (channels > 0 ? channels : 1));
                if (play_ms < 10) play_ms = 10;
                vTaskDelay(pdMS_TO_TICKS(play_ms));
            }


            size_t consumed = raw.consumed;
            if (consumed == 0) {
                consumed = 1;  //  Tiny files advance byte-by-byte
                if (consumed > remaining) consumed = remaining;
            }
        consecutive_errors = 0;  // ����ɹ������ô������
            input_ptr += consumed;
            remaining -= consumed;

        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer too small, needed %lu", (unsigned long)frame.needed_size);
            break;
        } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            ESP_LOGE(TAG, "Unsupported MP3 format (stopping)");
            break;
        } else {

            consecutive_errors++;
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "Too many consecutive decode errors (%d), aborting", consecutive_errors);
                break;
            }
            ESP_LOGW(TAG, "Decode error %d, skipping frame", ret);
            size_t skip = raw.consumed ? raw.consumed : 1;
            if (skip > remaining) skip = remaining;
            input_ptr += skip;
            remaining -= skip;
        }
    }


    if (mp3_dec_handle_) {
        esp_mp3_dec_close(mp3_dec_handle_);
        mp3_dec_handle_ = nullptr;
    }


    auto& app = Application::GetInstance();
    if (stop_requested_) {
        app.GetAudioService().FlushOutputDma();
    }
    app.GetAudioService().SetOutputMuted(false);

    ESP_LOGI(TAG, "Finished playing %s (stopped=%d)", filename, stop_requested_.load() ? 1 : 0);
    return !stop_requested_;
}

// ============================================


// ============================================
int Mp3Player::PlayUrl(const char* url) {
    if (!url || url[0] == '\0') return -1;

    ESP_LOGI(TAG, "PlayUrl: launching background task for %s", url);


    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PlayUrlCtx();
    ctx->self = this;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->url[sizeof(ctx->url) - 1] = '\0';

    xTaskCreate(PlayUrlTask, "mp3_url", 8192, ctx, 5, NULL);
        return 0;  // ������������
}

/**
 * @brief ͨ��HTTP������MP3�����ţ���̨�첽����
 * @param url HTTP URL
 * - �����أ����4MB�����ϸ�MPEG֡ͷУ�飬������������������+�ز�����24000Hz
 * - Ducking��AI˵��ʱ�Զ����͵�20%
 * - ��ʽ���룺����һ������һ����ѭ��ֱ������򱻴��
 */

static bool IsValidMpegHeader(const uint8_t* p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int layer = (p[1] >> 1) & 0x03;
    if (layer != 1) return false;  // Layer 3 only
    int bitrate = (p[2] >> 4) & 0x0F;
    if (bitrate == 0 || bitrate == 0x0F) return false;
    int srate = (p[2] >> 2) & 0x03;
    if (srate == 0x03) return false;
    return true;
}

void Mp3Player::PlayUrlTask(void* arg) {
    struct PlayUrlCtx { Mp3Player* self; char url[512]; };
    auto* ctx = (PlayUrlCtx*)arg;
    Mp3Player* self = ctx->self;
    char url[512];
    strncpy(url, ctx->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    delete ctx;

    ESP_LOGI(TAG, "PlayUrl: batch-stream task for %s", url);

    auto& app = Application::GetInstance();


    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "PlayUrl: client init failed"); vTaskDelete(NULL); return; }

    esp_err_t e = esp_http_client_open(client, 0);
    if (e != ESP_OK) { ESP_LOGE(TAG, "PlayUrl: open failed %d", e); esp_http_client_cleanup(client); vTaskDelete(NULL); return; }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "PlayUrl: HTTP %d, Len=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "PlayUrl: HTTP err %d", status);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }


    void* dec_handle = nullptr;
    esp_audio_err_t dec_ret = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PlayUrl: decoder fail %d", dec_ret);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        vTaskDelete(NULL); return;
    }

    self->is_playing_ = true;
    app.GetAudioService().SetOutputMuted(true);


    size_t batch_size = 256 * 1024;
    if (content_length > 0 && content_length < 8 * 1024 * 1024) {
        batch_size = content_length; // ȫ��һ������
        if (batch_size > 4 * 1024 * 1024) batch_size = 4 * 1024 * 1024; // ����4MB
        ESP_LOGI(TAG, "PlayUrl: batch=%dKB (content=%dKB)", (int)(batch_size/1024), content_length/1024);
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    if (!buf) {

        batch_size = 256 * 1024;
        buf = (uint8_t*)heap_caps_malloc(batch_size, MALLOC_CAP_SPIRAM);
    }
    if (!buf) {
        batch_size = 64 * 1024;
        buf = (uint8_t*)malloc(batch_size);  // last resort fallback
    }
    if (!buf) {
        ESP_LOGE(TAG, "PlayUrl: alloc failed");
        esp_mp3_dec_close(dec_handle); esp_http_client_close(client); esp_http_client_cleanup(client);
        self->is_playing_ = false; app.GetAudioService().SetOutputMuted(false);
        vTaskDelete(NULL); return;
    }


    const int kOutRate = 24000;
                // Buffer for worst-case MP3 frame (1152 stereo) upsampled from 8000->24000Hz


    const int kResampBufSamples = 4096;
    int16_t* resample_buf1 = (int16_t*)heap_caps_malloc(kResampBufSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t* resample_buf2 = (int16_t*)heap_caps_malloc(kResampBufSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!resample_buf1) resample_buf1 = (int16_t*)malloc(kResampBufSamples * sizeof(int16_t));
    if (!resample_buf2) resample_buf2 = (int16_t*)malloc(kResampBufSamples * sizeof(int16_t));
    ESP_LOGI(TAG, "PlayUrl: resamp bufs %s", (resample_buf1 && resample_buf2) ? "ok" : "WARN");

    esp_audio_dec_info_t dec_info = {};
    size_t total_dl = 0;
    int sample_rate = 0, play_ms = 0, batch_seq = 0;
    size_t mp3_start = 0, rem = 0;
    uint8_t* ptr = nullptr;

    // ============================================
    // 跳过 ID3v2 标签
    // ============================================
    size_t batch_len = 0;
    int err_cnt = 0;
    while (batch_len < batch_size && !self->stop_requested_) {
        int r = esp_http_client_read(client, (char*)(buf + batch_len), batch_size - batch_len);
        if (r > 0) { batch_len += r; err_cnt = 0; }
        else if (r == 0) break;
        else { if (++err_cnt > 3) break; vTaskDelay(pdMS_TO_TICKS(50)); }
    }
    total_dl = batch_len;
    if (batch_len == 0) goto cleanup;
    ESP_LOGI(TAG, "PlayUrl: batch#1 dl=%dKB", (int)(batch_len/1024));

    // 跳过 ID3v2 标签
    mp3_start = 0;
    if (batch_len > 10 && memcmp(buf, "ID3", 3) == 0) {
        uint32_t id3_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14)
                          | ((buf[8] & 0x7F) << 7)  |  (buf[9] & 0x7F);
        mp3_start = 10 + id3_size;
        if (mp3_start > batch_len - 1024) mp3_start = 0; // ��ǩ̫��/���ݲ��㣬��ͷ��ʼ
        ESP_LOGI(TAG, "PlayUrl: ID3v2 %luB, MP3 @ %u", id3_size, (unsigned)mp3_start);
    }


    for (size_t k = mp3_start; k + 3 < batch_len; k++) {
        if (buf[k] == 0xFF && (buf[k+1] & 0xE0) == 0xE0) {
            int ver = (buf[k+1] >> 3) & 0x03;
            int lay = (buf[k+1] >> 1) & 0x03;
            if (lay != 1) continue; // ֻҪ Layer 3
            int sri = (buf[k+2] >> 2) & 0x03;
            if      (ver == 3) { static const int r[]={44100,48000,32000,0}; sample_rate = r[sri]; }
            else if (ver == 2) { static const int r[]={22050,24000,16000,0}; sample_rate = r[sri]; }
            else if (ver == 0) { static const int r[]={11025,12000,8000,0};  sample_rate = r[sri]; }
            if (sample_rate > 0) { ESP_LOGI(TAG, "PlayUrl: MP3 %dHz @ %u", sample_rate, (unsigned)k); break; }
        }
    }
    if (sample_rate == 0) { sample_rate = 44100; ESP_LOGW(TAG, "PlayUrl: no sync header, default %dHz", sample_rate); }


    ptr = buf + mp3_start;
    rem = batch_len - mp3_start;
    batch_seq = 1;

    while (1) {
        while (rem > 0 && !self->stop_requested_) {
            size_t feed = (rem < self->kInputBufSize) ? rem : self->kInputBufSize;
            memcpy(self->input_buf_, ptr, feed);

            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = self->input_buf_;
            raw.len = feed;
            raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = self->output_buf_;
            frame.len = self->kOutputBufSize;

            dec_ret = esp_mp3_dec_decode(dec_handle, &raw, &frame, &dec_info);

            if (dec_ret == ESP_AUDIO_ERR_OK) {
                if (frame.decoded_size > 0 && !self->stop_requested_) {
 // ---- Smooth ducking: fade when AI starts speaking ----
                    auto dev_state = app.GetDeviceState();
                    if (dev_state == kDeviceStateSpeaking && self->ducking_gain_ >= 1.0f) {
                        self->ducking_gain_ = 1.0f;
                        self->ducking_start_us_ = esp_timer_get_time();
                        ESP_LOGI(TAG, "PlayUrl: AI speaking, fading out");
                    }
                    if (self->ducking_gain_ < 1.0f) {
                        if (dev_state != kDeviceStateSpeaking) {
                            self->ducking_gain_ = 1.0f; self->ducking_start_us_ = 0;
                        } else {
                            int64_t elapsed = esp_timer_get_time() - self->ducking_start_us_;
                            self->ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;
                            if (self->ducking_gain_ < 0.2f) {
                                self->ducking_gain_ = 0.2f;
                            }
                        }
                    }

                    int16_t* pcm = (int16_t*)frame.buffer;
                    size_t ns = frame.decoded_size / sizeof(int16_t);  // stereo sample count
                    size_t mono_ns = ns / 2;



                    if (sample_rate != kOutRate && mono_ns > 0) {
                    // Step 1: stereo -> mono (average L/R)
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }

                        float ratio = (float)kOutRate / (float)sample_rate;
                        size_t out_ns = (size_t)(mono_ns * ratio);
                        for (size_t i = 0; i < out_ns; i++) {
                            float sp = i / ratio;
                            size_t idx = (size_t)sp;
                            float frac = sp - idx;
                            if (idx + 1 < mono_ns)
                                resample_buf2[i] = (int16_t)(resample_buf1[idx] * (1.0f - frac) + resample_buf1[idx+1] * frac);
                            else
                                resample_buf2[i] = resample_buf1[idx];
                        }

                        float g = self->ducking_gain_;
                        for (size_t i = 0; i < out_ns; i++) {
                            int32_t s = (int32_t)(resample_buf2[i] * g);
                            if (s > 30000) s = 30000;
                            if (s < -30000) s = -30000;
                            resample_buf2[i] = (int16_t)s;
                        }
                        app.GetAudioService().OutputRawPcm(resample_buf2, out_ns, kOutRate);
                    } else {
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t sum = (int32_t)pcm[2*i] + (int32_t)pcm[2*i+1];
                            resample_buf1[i] = (int16_t)(sum / 2);
                        }

                        float g = self->ducking_gain_;
                        for (size_t i = 0; i < mono_ns; i++) {
                            int32_t s = (int32_t)(resample_buf1[i] * g);
                            if (s > 30000) s = 30000;
                            if (s < -30000) s = -30000;
                            resample_buf1[i] = (int16_t)s;
                        }
                        app.GetAudioService().OutputRawPcm(resample_buf1, mono_ns, sample_rate);
                    }
                    play_ms += (int)(ns * 1000 / sample_rate);
                }

                size_t c = dec_info.frame_size;
                if (c == 0) c = raw.consumed;  // �˻����
                if (c == 0) c = 1;
                if (c >= rem) { rem = 0; } else { ptr += c; rem -= c; }
            } else if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (raw.consumed > 0 && raw.consumed < rem) { ptr += raw.consumed; rem -= raw.consumed; }
                else if (raw.consumed == 0) {

                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 < rem) { ptr += scan; rem -= scan; }
                    else { rem = 0; break; }  // δ�ҵ�ͬ��֡ͷ������carryʣ��
                }
                if (rem < 1024) break;
            } else {
                if (raw.consumed > 0) {
                    if (raw.consumed >= rem) { rem = 0; }
                    else { ptr += raw.consumed; rem -= raw.consumed; }
                } else {

                    size_t scan = 1;
                    while (scan + 3 < rem && !IsValidMpegHeader(ptr + scan)) scan++;
                    if (scan + 3 >= rem) { rem = 0; break; }
                    ptr += scan; rem -= scan;
                }
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d rem=%u stop=%d", batch_seq, (unsigned)rem, self->stop_requested_.load() ? 1 : 0);
        if (self->stop_requested_) { printf("DD stop_break\n"); break; }


        size_t carry = rem;
        if (carry > 0 && carry < batch_size) {
            memmove(buf, ptr, carry);
        } else {
            carry = 0;
        }


        batch_len = 0; err_cnt = 0; batch_seq++;
        while (batch_len + carry < batch_size && !self->stop_requested_) {
            int r = esp_http_client_read(client, (char*)(buf + carry + batch_len), batch_size - carry - batch_len);
            if (r > 0) { batch_len += r; err_cnt = 0; }
            else if (r == 0) break;
            else { if (++err_cnt > 3) break; vTaskDelay(pdMS_TO_TICKS(50)); }
        }
        total_dl += batch_len;
        if (batch_len == 0) { ESP_LOGI(TAG, "PlayUrl: batch_len=0 break after %dKB", (int)(total_dl/1024)); break; }
        ESP_LOGD(TAG, "PlayUrl: batch#%d dl=%dKB+%uB carry", batch_seq, (int)(batch_len/1024), (unsigned)carry);
        ptr = buf;
        rem = carry + batch_len;

        if (carry == 0 && rem > 3 && !IsValidMpegHeader(ptr)) {
            size_t s = 1;
            while (s + 3 < rem && !IsValidMpegHeader(ptr + s)) s++;
            if (s + 3 < rem) {
                ESP_LOGD(TAG, "PlayUrl: skip %d bytes to next frame", (int)s);
                ptr += s; rem -= s;
            } else {
                rem = 0; // ��Ч֡ͷ������
            }
        }
        ESP_LOGD(TAG, "PlayUrl: batch#%d ready rem=%u %02X%02X%02X%02X...", batch_seq, (unsigned)rem,
               rem>0?ptr[0]:0, rem>1?ptr[1]:0, rem>2?ptr[2]:0, rem>3?ptr[3]:0);
    }
    ESP_LOGD(TAG, "PlayUrl: decode loop done rem=%u stop=%d", (unsigned)rem, self->stop_requested_.load() ? 1 : 0);

cleanup:

    free(buf);
    if (resample_buf1) free(resample_buf1);
    if (resample_buf2) free(resample_buf2);
    esp_mp3_dec_close(dec_handle);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    app.GetAudioService().SetOutputMuted(false);
    self->is_playing_ = false;

    ESP_LOGI(TAG, "PlayUrl: done %dms, dl=%dKB", play_ms, (int)(total_dl/1024));
    vTaskDelete(NULL);
}


/**


 * @return 0=�����ɹ�
 * - �ֿ����أ�4KB����PushRawPcmToPlayback���벥�Ŷ���
 * - Ducking��AI˵��ʱ�Զ�����������20%
 */
int Mp3Player::PlayPcm(const char* url) {
    if (is_playing_) {
        Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "PlayPcm: launching for %s", url);
    is_playing_ = true;

    struct PcmCtx { Mp3Player* self; char url[512]; };
    auto* ctx = new PcmCtx();
    ctx->self = this;
    strncpy(ctx->url, url, 511);
    ctx->url[511] = '\0';

    xTaskCreate(PlayPcmTask, "pcm_http", 8192, ctx, 3, NULL);
    return 0;
}

void Mp3Player::PlayPcmTask(void* arg) {
    struct PcmCtx { Mp3Player* self; char url[512]; };
    auto* ctx = (PcmCtx*)arg;
    auto* self = ctx->self;
    char url[512];
    strncpy(url, ctx->url, 511);
    url[511] = '\0';
    delete ctx;

    auto& app = Application::GetInstance();
    app.GetAudioService().SetOutputMuted(true);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.method = HTTP_METHOD_GET;
    http_cfg.timeout_ms = 15000;
    http_cfg.buffer_size = 4096;

    auto* client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "PlayPcm: client init failed");
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t e = esp_http_client_open(client, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "PlayPcm: open failed %d", e);
        esp_http_client_cleanup(client);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "PlayPcm: HTTP %d, Len=%d", status, content_length);

    if (status != 200) {
        esp_http_client_cleanup(client);
        self->is_playing_ = false;
        vTaskDelete(NULL);
        return;
    }


    const size_t CHUNK = sizeof(self->output_buf_);  // ��������������С
    const int SAMPLE_RATE = 24000;
    int64_t t0 = esp_timer_get_time();
    int bytes_yielded = 0;

    while (self->is_playing_ && !self->stop_requested_) {
        int read = esp_http_client_read(client, (char*)self->output_buf_, CHUNK);
        if (read <= 0) break;

        size_t samples = read / 2;  // 16λ������
        int16_t* pcm = (int16_t*)self->output_buf_;

 // ---- Smooth ducking: fade when AI starts speaking ----
        auto dev_state = app.GetDeviceState();
        if (dev_state == kDeviceStateSpeaking && self->ducking_gain_ >= 1.0f) {
            self->ducking_gain_ = 1.0f;
            self->ducking_start_us_ = esp_timer_get_time();
            ESP_LOGI(TAG, "PlayPcm: AI speaking, fading out");
        }
        if (self->ducking_gain_ < 1.0f) {
            if (dev_state != kDeviceStateSpeaking) {
                self->ducking_gain_ = 1.0f; self->ducking_start_us_ = 0;
            } else {
                int64_t elapsed = esp_timer_get_time() - self->ducking_start_us_;
                self->ducking_gain_ = 1.0f - 0.8f * (float)elapsed / 400000.0f;  // 400ms fade to 20%
                if (self->ducking_gain_ < 0.2f) {
                    self->ducking_gain_ = 0.2f;  // hold at 20% as background
                    ESP_LOGI(TAG, "PlayPcm: fade complete, holding at 20%%");
                }
            }
        }

        float g = self->ducking_gain_;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = (int32_t)(pcm[i] * g);
            if (s > 30000) s = 30000;
            if (s < -30000) s = -30000;
            pcm[i] = (int16_t)s;
        }

        app.GetAudioService().PushRawPcmToPlayback(pcm, samples, SAMPLE_RATE);


        bytes_yielded += read;
        if (bytes_yielded >= 192 * 1024) {  // ~4 seconds
            bytes_yielded = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    int64_t play_ms = (esp_timer_get_time() - t0) / 1000;
    self->is_playing_ = false;
    app.GetAudioService().SetOutputMuted(false);
    ESP_LOGI(TAG, "PlayPcm: done %lldms", play_ms);
    vTaskDelete(NULL);
}

/**
 * @brief ͨ��ԭʼBSD Socket����OGG/Opus��Ƶ�����͸�����������
 * @param url Opus��ƵURL
 * @return 0=�����ɹ�
 * - ʹ��BSD socketֱ�����ƹ�lwip esp_http_client


 * - Ducking��AI˵��ʱ��ͣ�������ݣ��������������ݣ���˵��ָ�
 * - �Զ��������͵�65%������AEC�زɸ��ţ�
 * - ���粻ͨʱ���˵�����ת��ģʽ
 */
int Mp3Player::PlayOpus(const char* url) {
    if (is_playing_) {
        ESP_LOGW(TAG, "PlayOpus: music already playing, refusing (call stop_music first)");
        return -1;
    }
    ESP_LOGI(TAG, "PlayOpus: launching for %s", url);
    is_playing_ = true;
    stop_requested_ = false;  // ��� Stop() ���õı�־
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
    
    // ԭʼBSD socket
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

        
 // === Serial fallback ===
        printf("\x01MUSIC_REQ\x02%s\x03\n", url);
        fflush(stdout);
        

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
            if (app.GetDeviceState() == kDeviceStateConnecting) break;  // ���Ѵʴ��� stop

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
    

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    send(sock, req, strlen(req), 0);
    

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
        

        if (codec) codec->SetOutputVolume(old_vol);
        ESP_LOGI(TAG, "PlayOpus: volume restored to %d", old_vol);
        

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

    app.GetAudioService().SetBackgroundAudioGain(0.001f);  // near-silent but keeps bg_audio_active_=true
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
 // Set gain based on current AI state before enabling drain
 // (prevents full-volume music + TTS overlap noise)
            auto state = app.GetDeviceState();
            // Duck only while AI actually talks; listening keeps full volume (user request 2026-07-19)
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            app.GetAudioService().EnableBgAudioDrain(true);
            ESP_LOGI(TAG, "PlayOpus: drain enabled (buffered %d samples in %d ms)",
                     (int)fill, (int)((esp_timer_get_time() - prebuf_start) / 1000));
            break;
        }
        if (esp_timer_get_time() - prebuf_start > 10000000) {
 // Set gain even on timeout otherwise stays at 0.001f
            auto state = app.GetDeviceState();
            bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
            ai_speaking = ai_now;
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
        // Listening keeps music at 100%; duck to 50% only while AI speaks/connects
        bool ai_now = (state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
        if (ai_now != ai_speaking) {
            ai_speaking = ai_now;
            app.GetAudioService().SetBackgroundAudioGain(ai_now ? 0.5f : 1.0f);
            int heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            int bg_fill = app.GetAudioService().GetBgAudioFillLevel();
            int task_hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "PlayOpus: AI %s, music -> %d%%, heap=%d sram=%d bg_buf=%d task_hwm=%d",
                     ai_now ? "speaking" : "quiet", ai_now ? 50 : 100,
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
        app.GetAudioService().SetBackgroundAudioGain(1.0f);
        while (app.GetAudioService().GetBgAudioFillLevel() > 0 && !self->stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    app.GetAudioService().ClearBackgroundAudio();
    close(sock);
    self->is_playing_ = false;
    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "PlayOpus: done %dms, dl=%dKB", (int)total_ms, (int)(total_dl / 1024));





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
 * @brief ��������ringtone
 * @param volume ����ϵ�� 0.0~1.0�����忪ͷ��15%��ǿ��100%

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
 * @brief ����������ѭ��
 * ��FreeRTOS��������ѯ pending_track_ / pending_bell_hour_��
 * ������ʱ����AI��������Ŷ�Ӧ��Ƶ�������ָ�AI���
 */

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

        PlayBell(track);
    } else if (folder == 2) {

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


    Stop();


    stop_requested_ = false;
    ducking_gain_ = 1.0f; ducking_start_us_ = 0;
    pending_track_ = index;


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


    pending_track_ = 0;
    pending_bell_hour_ = 0;

    if (mp3_dec_handle_) {
        esp_mp3_dec_reset(mp3_dec_handle_);
    }
    

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


    Stop();


    pending_bell_hour_ = hour;


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


    size_t max_samples = mp3_size * 4;
    if (max_samples < 8192) max_samples = 8192;
    if (max_samples > 2 * 1024 * 1024) max_samples = 2 * 1024 * 1024;  // cap at 2M samples (4MB)
    int16_t* buf = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu samples in PSRAM", (unsigned long)max_samples);
        return -1;
    }


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

// ============================================
LdrSensor::LdrSensor(gpio_num_t adc_pin, adc_unit_t unit, adc_channel_t chan, int threshold)