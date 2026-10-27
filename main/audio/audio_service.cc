#include "audio_service.h"
#include <esp_log.h>
#include <cstring>
#include <cmath>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 3,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"

// 开机阶段 codec/PSRAM 可能尚未完全就绪，创建解码器偶发失败（ret -1）。
// 失败后短暂重试，避免开机首条 E 日志和偶发「第一声没声」。
static bool OpenOpusDecoderWithRetry(esp_opus_dec_cfg_t* cfg,
                                     void** handle,
                                     int attempts = 3, int delay_ms = 50) {
    for (int i = 0; i < attempts; ++i) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        *handle = nullptr;
        esp_audio_err_t err = esp_opus_dec_open(
            cfg, sizeof(esp_opus_dec_cfg_t), handle);
        if (*handle != nullptr) {
            return true;
        }
        ESP_LOGW(TAG, "esp_opus_dec_open failed (attempt %d/%d), ret=%d", i + 1, attempts, (int)err);
    }
    return false;
}

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
    if (raw_pcm_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(raw_pcm_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    // Opus 解码器只支持 8/12/16/24/48kHz；设备输出 32kHz 不是合法解码率，
    // 直接用 output_sample_rate() 建解码器必然失败（ret -1）。
    // 默认按服务器常用的 24kHz 建，收到实际包时由 SetDecodeSampleRate() 重建。
    int default_decode_rate = codec->output_sample_rate();
    if (default_decode_rate != 8000 && default_decode_rate != 12000 &&
        default_decode_rate != 16000 && default_decode_rate != 24000 &&
        default_decode_rate != 48000) {
        default_decode_rate = 24000;
    }
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(default_decode_rate, OPUS_FRAME_DURATION_MS);
    if (!OpenOpusDecoderWithRetry(&opus_dec_cfg, &opus_decoder_)) {
        ESP_LOGE(TAG, "Failed to create audio decoder after retries");
    } else {
        decoder_sample_rate_ = default_decode_rate;
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
        // 默认解码率与设备输出率不同，先建好重采样器，避免首包同率时
        // SetDecodeSampleRate() 提前返回而忘了建重采样器
        if (decoder_sample_rate_ != codec->output_sample_rate()) {
            esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
                decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
            auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
            if (output_resampler_ == nullptr) {
                ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
            }
        }
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        // AEC 后电平统计（用于估算 ERLE）
        uint64_t sq = 0;
        for (auto s : data) {
            sq += (uint64_t)((int32_t)s * (int32_t)s);
        }
        aec_out_sq_.fetch_add(sq, std::memory_order_relaxed);
        aec_out_n_.fetch_add((uint32_t)data.size(), std::memory_order_relaxed);
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (speaking) {
            last_voice_time_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
        }
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    // Initialize background audio ring buffer (PSRAM)
    bg_audio_ring_.resize(BG_AUDIO_RING_SIZE, 0);
    bg_audio_write_pos_ = 0;
    bg_audio_read_pos_ = 0;
    bg_audio_active_ = false;

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    // 2026-09-11: 钉到 Core 1（保留）
    // 2026-10-24: 优先级 4 -> 19。Core 1 上 lwIP 任务是 18，聆听时上行/服务器
    // 消息会抢占输出任务，导致输出帧晚 ~19ms（I2S DMA 欠载、音乐变差）。
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 19, &audio_output_task_handle_, 1);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    // 2026-09-11: 钉到 Core 1（保留）
    // 2026-10-24: 优先级 4 -> 19（理由同上）
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 19, &audio_output_task_handle_, 1);
#endif

    /* Start the opus codec task */
    // 2026-09-11: 钉到 Core 1（保留）
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_, 1);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据给调试器
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word and/or audio processor */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                // Boost mic signal for better wake word sensitivity (INMP441 is quiet)
                for (auto& s : data) {
                    int32_t v = (int32_t)s * 2;
                    s = (v > 32767) ? 32767 : (v < -32768) ? -32768 : (int16_t)v;
                }
                {
                    std::lock_guard<std::mutex> lock(song_capture_mutex_);
                    if (song_capture_active_) {
                        const int channels = codec_->input_channels() > 0 ? codec_->input_channels() : 1;
                        for (size_t i = 0; i + channels <= data.size(); i += channels) {
                            song_capture_.push_back(data[i]);
                            if ((int)song_capture_.size() >= song_capture_target_) {
                                song_capture_active_ = false;
                                break;
                            }
                        }
                    }
                }
                // AEC 前后电平统计：每秒打印一行 ERLE（取麦克风声道）
                {
                    const int channels = codec_->input_channels() > 0 ? codec_->input_channels() : 1;
                    const size_t frames = data.size() / channels;
                    uint64_t sq = 0;
                    for (size_t i = 0; i < frames; i++) {
                        int32_t v = data[i * channels];
                        sq += (uint64_t)((int32_t)v * v);
                    }
                    aec_in_sq_.fetch_add(sq, std::memory_order_relaxed);
                    aec_in_n_.fetch_add((uint32_t)frames, std::memory_order_relaxed);
                    aec_stat_frames_ += (uint32_t)frames;
                    if (aec_stat_frames_ >= 16000) {
                        aec_stat_frames_ = 0;
                        uint64_t isq = aec_in_sq_.exchange(0);
                        uint32_t in_n = aec_in_n_.exchange(0);
                        uint64_t osq = aec_out_sq_.exchange(0);
                        uint32_t out_n = aec_out_n_.exchange(0);
                        if (in_n > 0 && out_n > 0) {
                            float in_rms = sqrtf((float)isq / (float)in_n);
                            // 扣除 AFE 输出端的 1.5x 数字增益（AFE_DIGITAL_GAIN）
                            float out_rms = sqrtf((float)osq / (float)out_n) / 1.5f;
                            float erle = (out_rms > 1.0f) ? 20.0f * log10f(in_rms / out_rms) : 0.0f;
                            ESP_LOGW(TAG, "AEC ERLE: in_rms=%.0f out_rms=%.0f erle=%.1f dB",
                                     in_rms, out_rms, erle);
                        }
                    }
                }
                if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
                    wake_word_->Feed(data);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
                    audio_processor_->Feed(std::move(data));
                }
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);

        // When background audio is active AND drain is enabled (e.g. online
        // music with enough data buffered), poll with a short timeout so we
        // can mix background audio into output even without AI TTS data.
        if (bg_audio_active_ && bg_audio_drain_enabled_) {
            // 10ms poll — shorter than 20ms silence frame so OutputData
            // backpressure keeps I2S DMA fed without gaps (prevent stutter)
            audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                return !audio_playback_queue_.empty() || service_stopped_;
            });
        } else {
            audio_queue_cv_.wait(lock, [this]() {
                return !audio_playback_queue_.empty() || service_stopped_ || bg_audio_drain_enabled_;
            });
        }

        if (service_stopped_) {
            break;
        }

        if (!audio_playback_queue_.empty()) {
            // Normal path: AI TTS data available
            auto task = std::move(audio_playback_queue_.front());
            audio_playback_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            if (!codec_->output_enabled()) {
                esp_timer_stop(audio_power_timer_);
                esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                codec_->EnableOutput(true);
            }

            if (output_muted_ && !task->bypass_mute) {
                // 静音时丢弃 Opus 解码的音频数据，不让其写 I2S
                continue;
            }
            MixBackgroundAudio(task->pcm);
            codec_->OutputData(task->pcm);

            bg_out_have_last_ = false;  // TTS 帧不参与 bg 帧间隔统计

            /* Update the last output time */
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;

        // DEBUG: count output samples
        i2s_out_samples_ += task->pcm.size();
        i2s_out_packets_++;
        if (i2s_out_packets_ % 20 == 0) {
            ESP_LOGI(TAG, "I2S-OUT: packets=%d samples=%d", i2s_out_packets_, i2s_out_samples_);
        }

#if CONFIG_USE_SERVER_AEC
            /* Record the timestamp for server AEC */
            if (task->timestamp > 0) {
                lock.lock();
                timestamp_queue_.push_back(task->timestamp);
            }
#endif
        } else {
            // Background-only path: queue is empty but background audio
            // ring buffer may have data. Generate a silent frame to host
            // the background mix so it reaches the speaker.
            lock.unlock();

            if (!codec_->output_enabled()) {
                esp_timer_stop(audio_power_timer_);
                esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                codec_->EnableOutput(true);
            }

            // 20 ms of silence at 16 kHz = host for MixBackgroundAudio
            // Static allocation avoids heap churn every 20ms which can
            // cause timing jitter and worsen audio stutter on underrun
            static constexpr int kBgFrameSamples = 32000 * 20 / 1000;
            static std::vector<int16_t> bg_silence(kBgFrameSamples);
            std::fill(bg_silence.begin(), bg_silence.end(), 0);

            // 诊断：把帧间隔拆成 A（等待/调度）+ 混音 + B（I2S 写入）
            auto t_start = std::chrono::steady_clock::now();
            if (bg_out_have_last_) {
                auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  t_start - bg_out_last_time_).count();
                if (gap_ms > 30) {
                    bg_late_frames_.fetch_add(1, std::memory_order_relaxed);
                    if ((uint32_t)gap_ms > bg_max_gap_ms_.load(std::memory_order_relaxed)) {
                        bg_max_gap_ms_.store((uint32_t)gap_ms, std::memory_order_relaxed);
                    }
                }
                auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   t_start - bg_out_last_end_).count();
                if (wait_ms > 30) {
                    bg_wait_late_frames_.fetch_add(1, std::memory_order_relaxed);
                    if ((uint32_t)wait_ms > bg_max_wait_ms_.load(std::memory_order_relaxed)) {
                        bg_max_wait_ms_.store((uint32_t)wait_ms, std::memory_order_relaxed);
                    }
                }
            }

            MixBackgroundAudio(bg_silence);
            auto t_write_start = std::chrono::steady_clock::now();
            codec_->OutputData(bg_silence);
            auto t_end = std::chrono::steady_clock::now();
            auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                t_end - t_write_start).count();
            if (write_ms > 25) {
                bg_write_late_frames_.fetch_add(1, std::memory_order_relaxed);
                if ((uint32_t)write_ms > bg_max_write_ms_.load(std::memory_order_relaxed)) {
                    bg_max_write_ms_.store((uint32_t)write_ms, std::memory_order_relaxed);
                }
            }
            bg_out_last_time_ = t_start;
            bg_out_last_end_ = t_end;
            bg_out_have_last_ = true;

            /* Prevent power management from disabling output */
            last_output_time_ = std::chrono::steady_clock::now();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            int sample_rate = packet->sample_rate;
            if (sample_rate > codec_->output_sample_rate()) {
                sample_rate = codec_->output_sample_rate();
            }
            SetDecodeSampleRate(sample_rate, packet->frame_duration);
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    lock.lock();
                    audio_playback_queue_.push_back(std::move(task));
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                    lock.lock();
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        // 诊断：uplink_muted_ 为真时只丢弃上行包，麦克风采集/AFE 照常运行
                        if (!uplink_muted_.load(std::memory_order_relaxed)) {
                            {
                                std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                                audio_send_queue_.push_back(std::move(packet));
                            }
                            if (callbacks_.on_send_queue_available) {
                                callbacks_.on_send_queue_available();
                            }
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    if (!OpenOpusDecoderWithRetry(&opus_dec_cfg, &opus_decoder_)) {
        ESP_LOGE(TAG, "Failed to create audio decoder after retries");
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            decode_dropped_++;
            return false;
        }
    }
    decode_pushed_++;
    if (decode_pushed_ % 20 == 0 || decode_dropped_ > 0) {
        ESP_LOGI(TAG, "DECODE-Q: pushed=%d dropped=%d q_size=%d", decode_pushed_, decode_dropped_, audio_decode_queue_.size());
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        // Echo guard: don't enable wake word while speaker audio is still
        // echoing in the room. Without this, TTS output gets picked up by the mic,
        // triggering wake word and causing echo loops (AI talks to itself).
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
        if (elapsed < 300) {
            vTaskDelay(pdMS_TO_TICKS(300 - elapsed));
        }

        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // Reset input resampler to clear cached data from previous mode (e.g. WakeWord)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    ESP_LOGI(TAG, "WAIT-PLAYBACK: dq=%d pq=%d pushed=%d dropped=%d",
        audio_decode_queue_.size(), audio_playback_queue_.size(), decode_pushed_, decode_dropped_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_ || (audio_decode_queue_.empty() && audio_playback_queue_.empty()); 
    });
    ESP_LOGI(TAG, "WAIT-DONE: dq=%d pq=%d pushed=%d dropped=%d",
        audio_decode_queue_.size(), audio_playback_queue_.size(), decode_pushed_, decode_dropped_);
}


void AudioService::SetUplinkMuted(bool muted) {
    uplink_muted_.store(muted, std::memory_order_relaxed);
    if (muted) {
        // 立即丢掉已排队的上行包，让 OFF 窗口马上生效（不等队列排空）
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_send_queue_.clear();
    }
}

void AudioService::SetOutputMuted(bool muted) {
    ESP_LOGI(TAG, "SET-MUTED: %d (dq=%d pq=%d pushed=%d dropped=%d)", muted,
        audio_decode_queue_.size(), audio_playback_queue_.size(), decode_pushed_, decode_dropped_);
    output_muted_ = muted;
    if (muted) {
        // 清空 playback queue，防止排队的音频在恢复后突然播放
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_playback_queue_.clear();
        ESP_LOGI("AudioService", "Output muted, playback queue cleared");
    }
}

void AudioService::FlushOutputDma() {
    // disable I2S TX 通道 → 清空 DMA FIFO
    codec_->EnableOutput(false);
    // 立即重新使能，muted 状态下 audio_output_task 不会写新数据
    codec_->EnableOutput(true);
    // 给 I2S 一点时间稳定
    vTaskDelay(pdMS_TO_TICKS(5));
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    ESP_LOGI(TAG, "RESET-DECODER: dq=%d pq=%d pushed=%d dropped=%d i2s_pkts=%d i2s_samp=%d",
        audio_decode_queue_.size(), audio_playback_queue_.size(), decode_pushed_, decode_dropped_,
        i2s_out_packets_, i2s_out_samples_);
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
    decode_pushed_ = 0;
    decode_dropped_ = 0;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            ESP_LOGI(TAG, "POWER-OFF: output idle %lld ms", output_elapsed);
            codec_->EnableOutput(false);
        } else {
            ESP_LOGD(TAG, "POWER-KEEP: duplex+RX active, keeping TX clock");
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (models_list_ != nullptr && esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (models_list_ != nullptr && esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else if (models_list_ == nullptr) {
        // No assets partition (e.g. cuckoo-clock board), try to init model directly
#ifdef CONFIG_USE_CUSTOM_WAKE_WORD
        wake_word_ = std::make_unique<CustomWakeWord>();
#else
        wake_word_ = nullptr;
#endif
    } else {
        wake_word_ = nullptr;
    }
#else
    if (models_list_ != nullptr && esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
void AudioService::PushRawPcmToPlayback(const int16_t* data, size_t num_samples, int sample_rate) {
    // 包装为 AudioTask 推入播放队列，由 AudioOutputTask 串行化写入 I2S
    // 避免和 Opus 音频路径争抢 codec_->OutputData()
    auto task = std::make_unique<AudioTask>();
    // 先重采样到 codec 的输出采样率，因为 AudioOutputTask 不做重采样
    if (sample_rate != codec_->output_sample_rate()) {
        float ratio = (float)codec_->output_sample_rate() / (float)sample_rate;
        size_t out_samples = (size_t)(num_samples * ratio);
        task->pcm.resize(out_samples);
        for (size_t i = 0; i < out_samples; i++) {
            float src_pos = i / ratio;
            size_t idx = (size_t)src_pos;
            float frac = src_pos - idx;
            if (idx + 1 < num_samples) {
                task->pcm[i] = (int16_t)(data[idx] * (1.0f - frac) + data[idx + 1] * frac);
            } else {
                task->pcm[i] = data[idx];
            }
        }
    } else {
        task->pcm.assign(data, data + num_samples);
    }
    task->type = kAudioTaskTypeDecodeToPlaybackQueue;
    task->bypass_mute = true;  // 钟声应绕过 output_muted_ 检查

    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_playback_queue_.push_back(std::move(task));
    }
    audio_queue_cv_.notify_all();
}

void AudioService::OutputRawPcm(const int16_t* data, size_t num_samples, int sample_rate) {
    if (!codec_) return;
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }
    int out_rate = codec_->output_sample_rate();
    ESP_LOGI(TAG, "OutputRawPcm: samples=%d in_rate=%d out_rate=%d",
             (int)num_samples, sample_rate, out_rate);
    if (sample_rate != out_rate) {
        // 使用 ESP AE Rate Converter（高质量 FIR 重采样，复杂度 3），替代手写线性插值
        if (raw_pcm_resampler_ == nullptr || raw_pcm_src_rate_ != sample_rate) {
            if (raw_pcm_resampler_ != nullptr) {
                esp_ae_rate_cvt_close(raw_pcm_resampler_);
            }
            auto cfg = RATE_CVT_CFG(sample_rate, out_rate, 1);
            esp_ae_rate_cvt_open(&cfg, &raw_pcm_resampler_);
            raw_pcm_src_rate_ = sample_rate;
        }
        uint32_t out_samples = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(raw_pcm_resampler_, num_samples, &out_samples);
        std::vector<int16_t> resampled(out_samples);
        uint32_t actual = out_samples;
        esp_ae_rate_cvt_process(raw_pcm_resampler_, (esp_ae_sample_t)data, num_samples,
                               (esp_ae_sample_t)resampled.data(), &actual);
        resampled.resize(actual);
        codec_->OutputData(resampled);
    } else {
        std::vector<int16_t> vec(data, data + num_samples);
        codec_->OutputData(vec);
    }
    last_output_time_ = std::chrono::steady_clock::now();
    debug_statistics_.playback_count++;
}

void AudioService::StartSongCapture(int duration_ms) {
    if (duration_ms < 1000) duration_ms = 1000;
    if (duration_ms > 10000) duration_ms = 10000;
    std::lock_guard<std::mutex> lock(song_capture_mutex_);
    song_capture_.clear();
    song_capture_target_ = 16000 * duration_ms / 1000;
    song_capture_active_ = true;
}

void AudioService::StopSongCapture() {
    std::lock_guard<std::mutex> lock(song_capture_mutex_);
    song_capture_active_ = false;
}

bool AudioService::GetSongCapture(std::vector<int16_t>& out) {
    std::lock_guard<std::mutex> lock(song_capture_mutex_);
    if (song_capture_.empty()) return false;
    if (song_capture_active_ &&
        (int)song_capture_.size() < song_capture_target_) {
        return false;
    }
    out = std::move(song_capture_);
    song_capture_.clear();
    song_capture_active_ = false;
    return true;
}

void AudioService::PushBackgroundAudio(const int16_t* data, size_t samples, int sample_rate) {
    if (!bg_audio_active_ || sample_rate != 32000) return;
    std::lock_guard<std::mutex> lock(bg_audio_mutex_);
    for (size_t i = 0; i < samples; i++) {
        bg_audio_ring_[bg_audio_write_pos_] = data[i];
        bg_audio_write_pos_ = (bg_audio_write_pos_ + 1) % BG_AUDIO_RING_SIZE;
        // If ring is full, advance read pos (drop oldest sample)
        if (bg_audio_write_pos_ == bg_audio_read_pos_) {
            bg_audio_read_pos_ = (bg_audio_read_pos_ + 1) % BG_AUDIO_RING_SIZE;
        }
    }
}

void AudioService::ResetMusicDecoder() {
    std::lock_guard<std::mutex> lock(music_decoder_mutex_);
    if (music_opus_decoder_ != nullptr) {
        esp_opus_dec_close(music_opus_decoder_);
        music_opus_decoder_ = nullptr;
    }
    if (music_output_resampler_ != nullptr) {
        esp_ae_rate_cvt_reset(music_output_resampler_);
    }
    music_decoder_sample_rate_ = 0;
    music_decoder_duration_ms_ = 0;
    music_decoder_frame_size_ = 0;
}

bool AudioService::PushMusicOpusPacket(const uint8_t* data, size_t len, int sample_rate, int frame_duration) {
    if (data == nullptr || len == 0 || sample_rate <= 0) return false;
    if (frame_duration <= 0) frame_duration = 60;

    std::lock_guard<std::mutex> lock(music_decoder_mutex_);
    if (music_opus_decoder_ == nullptr || music_decoder_sample_rate_ != sample_rate ||
        music_decoder_duration_ms_ != frame_duration) {
        if (music_opus_decoder_ != nullptr) {
            esp_opus_dec_close(music_opus_decoder_);
            music_opus_decoder_ = nullptr;
        }
        esp_opus_dec_cfg_t cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
        auto ret = esp_opus_dec_open(&cfg, sizeof(esp_opus_dec_cfg_t), &music_opus_decoder_);
        if (music_opus_decoder_ == nullptr) {
            ESP_LOGE(TAG, "Music decoder open failed: %d (rate=%d dur=%d)", ret, sample_rate, frame_duration);
            return false;
        }
        music_decoder_sample_rate_ = sample_rate;
        music_decoder_duration_ms_ = frame_duration;
        music_decoder_frame_size_ = sample_rate / 1000 * frame_duration;
        ESP_LOGI(TAG, "Music decoder: %d Hz, %d ms, %d samples/frame",
                 sample_rate, frame_duration, music_decoder_frame_size_);
    }

    std::vector<int16_t> pcm((size_t)music_decoder_frame_size_);
    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t*)data,
        .len = (uint32_t)len,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = (uint8_t*)pcm.data(),
        .len = (uint32_t)(pcm.size() * sizeof(int16_t)),
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {};
    auto ret = esp_opus_dec_decode(music_opus_decoder_, &raw, &out, &info);
    if (ret != ESP_AUDIO_ERR_OK || out.decoded_size == 0) {
        ESP_LOGW(TAG, "Music decode failed: %d", ret);
        return false;
    }
    pcm.resize(out.decoded_size / sizeof(int16_t));

    int out_rate = codec_ ? codec_->output_sample_rate() : 32000;
    if (sample_rate != out_rate) {
        if (music_output_resampler_ == nullptr || music_resampler_src_rate_ != sample_rate) {
            if (music_output_resampler_ != nullptr) {
                esp_ae_rate_cvt_close(music_output_resampler_);
                music_output_resampler_ = nullptr;
            }
            esp_ae_rate_cvt_cfg_t rcfg = RATE_CVT_CFG(sample_rate, out_rate, ESP_AUDIO_MONO);
            auto rret = esp_ae_rate_cvt_open(&rcfg, &music_output_resampler_);
            if (music_output_resampler_ == nullptr) {
                ESP_LOGE(TAG, "Music resampler open failed: %d", rret);
                return false;
            }
            music_resampler_src_rate_ = sample_rate;
        }
        uint32_t target = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(music_output_resampler_, pcm.size(), &target);
        std::vector<int16_t> resampled(target);
        uint32_t actual = target;
        esp_ae_rate_cvt_process(music_output_resampler_, (esp_ae_sample_t)pcm.data(), pcm.size(),
                                (esp_ae_sample_t)resampled.data(), &actual);
        resampled.resize(actual);
        PushBackgroundAudio(resampled.data(), resampled.size(), out_rate);
    } else {
        PushBackgroundAudio(pcm.data(), pcm.size(), out_rate);
    }
    return true;
}

void AudioService::MixIntoBackgroundAudio(const int16_t* data, size_t samples, float gain) {
    if (!bg_audio_active_) return;
    std::lock_guard<std::mutex> lock(bg_audio_mutex_);
    size_t pos = bg_audio_read_pos_;
    for (size_t i = 0; i < samples && pos != bg_audio_write_pos_; i++) {
        int32_t mixed = (int32_t)bg_audio_ring_[pos] + (int32_t)((float)data[i] * gain);
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        bg_audio_ring_[pos] = (int16_t)mixed;
        pos = (pos + 1) % BG_AUDIO_RING_SIZE;
    }
}

void AudioService::SetBackgroundAudioGain(float gain) {
    bool was_active = bg_audio_active_;
    bg_audio_target_gain_ = gain;
    bg_audio_active_ = (gain > 0.0f);
    if (bg_audio_active_) {
        RefreshOutputTimestamp();
        if (!was_active) {
            std::lock_guard<std::mutex> lock(bg_audio_mutex_);
            // Reset ring buffer only on inactive→active transition.
            // Changing gain while already active (e.g. ducking 1.0→0.3
            // or restoring 0.3→1.0) must NOT discard buffered data.
            bg_audio_read_pos_ = bg_audio_write_pos_;
            bg_audio_gain_ = gain;
        }
    }
}

void AudioService::ClearBackgroundAudio() {
    bg_audio_active_ = false;
    bg_audio_drain_enabled_ = false;
    std::lock_guard<std::mutex> lock(bg_audio_mutex_);
    bg_audio_read_pos_ = bg_audio_write_pos_;
}

void AudioService::EnableBgAudioDrain(bool enable) {
    bg_audio_drain_enabled_ = enable;
    if (enable) {
        // Wake the output task so it switches from blocking wait to
        // poll mode immediately (no 20ms delay on first frame)
        audio_queue_cv_.notify_all();
    }
}

size_t AudioService::GetBgAudioFillLevel() {
    std::lock_guard<std::mutex> lock(bg_audio_mutex_);
    if (bg_audio_write_pos_ >= bg_audio_read_pos_) {
        return bg_audio_write_pos_ - bg_audio_read_pos_;
    }
    return BG_AUDIO_RING_SIZE - bg_audio_read_pos_ + bg_audio_write_pos_;
}

void AudioService::MixBackgroundAudio(std::vector<int16_t>& pcm) {
    if (!bg_audio_active_ && bg_audio_gain_ <= 0.0f) return;
    std::lock_guard<std::mutex> lock(bg_audio_mutex_);
    bg_mix_frames_.fetch_add(1, std::memory_order_relaxed);
    
    size_t available;
    if (bg_audio_write_pos_ >= bg_audio_read_pos_) {
        available = bg_audio_write_pos_ - bg_audio_read_pos_;
    } else {
        available = BG_AUDIO_RING_SIZE - bg_audio_read_pos_ + bg_audio_write_pos_;
    }
    
    if (available == 0) {
        // 诊断：这一帧完全没数据 → 欠载（听感上就是短暂塌陷）
        bg_underrun_frames_.fetch_add(1, std::memory_order_relaxed);
        bg_underrun_samples_.fetch_add(pcm.size(), std::memory_order_relaxed);
        // Still ramp gain toward target even with no data
        if (bg_audio_gain_ != bg_audio_target_gain_) {
            constexpr float kFadeStep = 0.7f / 4800.0f;  // 300ms ramp at 16kHz
            float step = kFadeStep * (float)pcm.size();
            if (bg_audio_gain_ < bg_audio_target_gain_) {
                bg_audio_gain_ += step;
                if (bg_audio_gain_ > bg_audio_target_gain_) bg_audio_gain_ = bg_audio_target_gain_;
            } else {
                bg_audio_gain_ -= step;
                if (bg_audio_gain_ < bg_audio_target_gain_) bg_audio_gain_ = bg_audio_target_gain_;
            }
        }
        return;
    }
    size_t mix_count = (pcm.size() < available) ? pcm.size() : available;
    
    // Per-sample gain ramping (fade over ~16000 samples = 1000ms at 16kHz)
    constexpr float kFadeStep = 0.7f / 16000.0f;
    float gain = bg_audio_gain_;
    float target = bg_audio_target_gain_;

    // 诊断：本窗口的音乐/输出峰值与硬钳位数量
    uint32_t music_peak = 0, out_peak = 0, clip_count = 0;
    
    for (size_t i = 0; i < mix_count; i++) {
        if (gain < target) {
            gain += kFadeStep;
            if (gain > target) gain = target;
        } else if (gain > target) {
            gain -= kFadeStep;
            if (gain < target) gain = target;
        }
        
        int32_t music = (int32_t)(bg_audio_ring_[bg_audio_read_pos_] * gain);
        uint32_t am = (uint32_t)(music >= 0 ? music : -music);
        if (am > music_peak) music_peak = am;
        int32_t mixed = (int32_t)pcm[i] + music;
        if (mixed > 32767) {
            mixed = 32767;
            clip_count++;
        } else if (mixed < -32768) {
            mixed = -32768;
            clip_count++;
        }
        uint32_t ao = (uint32_t)(mixed >= 0 ? mixed : -mixed);
        if (ao > out_peak) out_peak = ao;
        pcm[i] = (int16_t)mixed;
        bg_audio_read_pos_ = (bg_audio_read_pos_ + 1) % BG_AUDIO_RING_SIZE;
    }

    if (clip_count > 0) {
        bg_clip_samples_.fetch_add(clip_count, std::memory_order_relaxed);
    }
    if (music_peak > bg_music_peak_.load(std::memory_order_relaxed)) {
        bg_music_peak_.store(music_peak, std::memory_order_relaxed);
    }
    if (out_peak > bg_out_peak_.load(std::memory_order_relaxed)) {
        bg_out_peak_.store(out_peak, std::memory_order_relaxed);
    }
    
    // Underrun protection: if ring buffer ran dry mid-frame,
    // crossfade the last mixed sample to zero instead of a hard cut
    // (which would produce an audible click/pop)
    if (mix_count < pcm.size() && mix_count > 0) {
        // 诊断：半帧欠载（本帧后半段无数据，交叉淡出掩盖）
        bg_underrun_frames_.fetch_add(1, std::memory_order_relaxed);
        bg_underrun_samples_.fetch_add(pcm.size() - mix_count, std::memory_order_relaxed);
        int16_t last_val = pcm[mix_count - 1];
        size_t remaining = pcm.size() - mix_count;
        for (size_t i = 0; i < remaining; i++) {
            float fade = 1.0f - (float)(i + 1) / (float)(remaining + 1);
            pcm[mix_count + i] = (int16_t)((float)last_val * fade);
        }
    }
    
    bg_audio_gain_ = gain;
}
