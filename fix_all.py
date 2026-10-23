fname = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_part4.cc'

with open(fname, 'rb') as f:
    data = f.read()

# Function 1: Replace PlayShowMusicBg
# Find: "bool CuckooStateMachine::PlayShowMusicBg(int index) {\r\n    if (!mp3_) return false;"
old_start = b'bool CuckooStateMachine::PlayShowMusicBg(int index) {\r\n    if (!mp3_) return false;'
# Find end: the closing "return true;\r\n}" before StartLindaShow
old_end = b'return true;\r\n}\r\n\r\n/**\r\n * @brief'

# Find the function
idx_start = data.find(old_start)
if idx_start == -1:
    print('ERROR: PlayShowMusicBg start not found')
else:
    print(f'Found PlayShowMusicBg at offset {idx_start}')
    # Find the end from there
    idx_end_offset = data.find(old_end, idx_start)
    if idx_end_offset == -1:
        print('ERROR: PlayShowMusicBg end not found')
    else:
        idx_end = idx_end_offset + len(b'return true;\r\n}')
        print(f'End at offset {idx_end}, function is {idx_end - idx_start} bytes')

        new_func = b'bool CuckooStateMachine::PlayShowMusicBg(int index) {\r\n    if (!mp3_) return false;\r\n\r\n    auto& audio = Application::GetInstance().GetAudioService();\r\n    audio.SetBackgroundAudioGain(1.0f);\r\n\r\n    struct StreamCtx {\r\n        AudioService* audio;\r\n        std::atomic<bool>* is_running;\r\n        size_t total_pushed;\r\n        bool drain_enabled;\r\n    } ctx = {&audio, &is_running_, 0, false};\r\n\r\n    int ret = mp3_->DecodeStreaming(index, [](const int16_t* pcm, size_t samples, int src_sr, void* user_data) {\r\n        auto* c = (StreamCtx*)user_data;\r\n        size_t offset = 0;\r\n        while (offset < samples && c->is_running->load()) {\r\n            size_t chunk = (samples - offset > 2048) ? 2048 : (samples - offset);\r\n            if (src_sr != 16000) {\r\n                float ratio = (float)src_sr / 16000;\r\n                std::vector<int16_t> dst(chunk * 2 + 4);\r\n                size_t d = 0; float pos = 0;\r\n                while (pos < (float)chunk - 1.0f && d < dst.size() - 1) {\r\n                    size_t idx = (size_t)pos;\r\n                    float frac = pos - idx;\r\n                    dst[d++] = (int16_t)((float)pcm[offset + idx] * (1.0f - frac) + (float)pcm[offset + idx + 1] * frac);\r\n                    pos += ratio;\r\n                }\r\n                c->audio->PushBackgroundAudio(dst.data(), d, 16000);\r\n            } else {\r\n                c->audio->PushBackgroundAudio(pcm + offset, chunk, 16000);\r\n            }\r\n            c->total_pushed += chunk;\r\n            if (!c->drain_enabled && c->total_pushed >= 64000) {\r\n                c->audio->EnableBgAudioDrain(true);\r\n                c->drain_enabled = true;\r\n            }\r\n            offset += chunk;\r\n            while (c->audio->GetBgAudioFillLevel() > 64000 && c->is_running->load())\r\n                vTaskDelay(pdMS_TO_TICKS(50));\r\n        }\r\n    }, &ctx);\r\n\r\n    if (ret < 0) {\r\n        ESP_LOGE(TAG, "PlayShowMusicBg: decode failed for %04d.mp3", index);\r\n        return false;\r\n    }\r\n    int drain_wait = 0;\r\n    while (is_running_ && audio.GetBgAudioFillLevel() > 0 && drain_wait < 200) {\r\n        vTaskDelay(pdMS_TO_TICKS(50)); drain_wait++;\r\n    }\r\n    vTaskDelay(pdMS_TO_TICKS(200));\r\n    audio.ClearBackgroundAudio();\r\n    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 finished, bg audio cleared", index);\r\n    return true;\r\n}'

        data = data[:idx_start] + new_func + data[idx_end:]

# Function 2: Remove water_bird_->Stop() from BirdJumpPulse
old_bp = b'    if (water_bird_) water_bird_->Stop();\r\n    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);\r\n    vTaskDelay(pdMS_TO_TICKS(500));\r\n    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);'
new_bp = b'    gpio_set_level(MOTOR_WATER_BIRD_IN2, 1);\r\n    vTaskDelay(pdMS_TO_TICKS(500));\r\n    gpio_set_level(MOTOR_WATER_BIRD_IN2, 0);'
if new_bp in data:
    print('BirdJumpPulse already fixed')
else:
    count = data.replace(old_bp, new_bp, 1)
    if len(count) != len(data):
        data = count
        print('Removed water_bird_->Stop() from BirdJumpPulse')
    else:
        print('WARNING: BirdJumpPulse Stop() not found')

with open(fname, 'wb') as f:
    f.write(data)
print('Done')
