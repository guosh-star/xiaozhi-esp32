import sys, io

base = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock'

with open(base + r'\cuckoo_part4.cc', 'rb') as f:
    data = f.read()
content = data.decode('utf-8', errors='replace')
lines = content.split('\n')

# Find PlayShowMusicBg start and end
start = -1
for i, l in enumerate(lines):
    if 'bool CuckooStateMachine::PlayShowMusicBg(int index)' in l:
        start = i
        break

if start == -1:
    print('ERROR: PlayShowMusicBg not found')
    sys.exit(1)

# Find matching closing brace
brace = 0
end = start
for i in range(start, len(lines)):
    brace += lines[i].count('{')
    brace -= lines[i].count('}')
    if brace == 0 and i > start + 2:
        end = i
        break

print(f'PlayShowMusicBg: lines {start+1}-{end+1} ({end-start+1} lines)')

new_func = r'''bool CuckooStateMachine::PlayShowMusicBg(int index) {
    if (!mp3_) return false;

    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetBackgroundAudioGain(1.0f);

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

            if (src_sr != 16000) {
                float ratio = (float)src_sr / 16000;
                std::vector<int16_t> dst(chunk * 2 + 4);
                size_t d = 0;
                float pos = 0;
                while (pos < (float)chunk - 1.0f && d < dst.size() - 1) {
                    size_t idx = (size_t)pos;
                    float frac = pos - idx;
                    dst[d++] = (int16_t)((float)pcm[offset + idx] * (1.0f - frac) + (float)pcm[offset + idx + 1] * frac);
                    pos += ratio;
                }
                c->audio->PushBackgroundAudio(dst.data(), d, 16000);
            } else {
                c->audio->PushBackgroundAudio(pcm + offset, chunk, 16000);
            }

            c->total_pushed += chunk;
            if (!c->drain_enabled && c->total_pushed >= 64000) {
                c->audio->EnableBgAudioDrain(true);
                c->drain_enabled = true;
            }
            offset += chunk;

            while (c->audio->GetBgAudioFillLevel() > 64000 && c->is_running->load())
                vTaskDelay(pdMS_TO_TICKS(50));
        }
    }, &ctx);

    if (ret < 0) {
        ESP_LOGE(TAG, "PlayShowMusicBg: decode failed for %04d.mp3", index);
        return false;
    }

    int drain_wait = 0;
    while (is_running_ && audio.GetBgAudioFillLevel() > 0 && drain_wait < 200) {
        vTaskDelay(pdMS_TO_TICKS(50));
        drain_wait++;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    audio.ClearBackgroundAudio();
    ESP_LOGI(TAG, "PlayShowMusicBg: %04d.mp3 finished, bg audio cleared", index);
    return true;
}'''

new_lines = new_func.split('\n')
if new_lines[-1] == '':
    new_lines = new_lines[:-1]

old_count = end - start + 1
result = lines[:start] + new_lines + lines[end+1:]
net = len(new_lines) - old_count
print(f'Replaced {old_count} lines with {len(new_lines)} lines (net {net:+d})')

# Fix BirdJumpPulse: remove water_bird_->Stop()
for i, l in enumerate(result):
    if 'void CuckooStateMachine::BirdJumpPulse()' in l:
        for j in range(i+1, min(i+8, len(result))):
            if 'water_bird_->Stop()' in result[j]:
                print(f'Removed water_bird_->Stop() at line {j+1}: {result[j].strip()}')
                del result[j]
                break
        break

# Write back with \r\n line endings
out = '\r\n'.join(result)
with open(base + r'\cuckoo_part4.cc', 'wb') as f:
    f.write(out.encode('utf-8'))
print(f'Done! File has {len(result)} lines, {len(out)} bytes')
