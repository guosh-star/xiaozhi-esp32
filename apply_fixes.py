import sys

with open('clean_controller.cc', 'rb') as f:
    text = f.read()[2:].decode('utf-16-le')
lines = text.split('\n')

# Fix 1: After LoadKidsActive block, add SaveHourlyPerf/LoadHourlyPerf
for i in range(len(lines)):
    if i < len(lines) and 'nvs_close' in lines[i] and i > 2000 and i < 2700:
        for j in range(i, i+5):
            if j < len(lines) and lines[j].strip() == '}':
                insert_at = j + 1
                new_code = [
                    '',
                    'void CuckooStateMachine::LoadHourlyPerf() {',
                    '    nvs_handle_t nvs;',
                    '    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {',
                    '        int8_t val;',
                    '        if (nvs_get_i8(nvs, "hperf", &val) == ESP_OK) hourly_perf_ = (bool)val;',
                    '        nvs_close(nvs);',
                    '    }',
                    '}',
                    '',
                    'void CuckooStateMachine::SaveHourlyPerf() {',
                    '    nvs_handle_t nvs;',
                    '    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {',
                    '        nvs_set_i8(nvs, "hperf", (int8_t)hourly_perf_.load());',
                    '        nvs_commit(nvs);',
                    '        nvs_close(nvs);',
                    '    }',
                    '}',
                ]
                lines[insert_at:insert_at] = new_code
                print('Fix 1: Added SaveHourlyPerf/LoadHourlyPerf')
                break
        break

# Fix 2: LoadHourlyPerf() call after LoadKidsActive() call
for i in range(len(lines)):
    if 'sm->LoadKidsActive()' in lines[i]:
        lines.insert(i+1, '    sm->LoadHourlyPerf();')
        print('Fix 2: Added LoadHourlyPerf() call')
        break

# Fix 3: SaveAlarmsToNvs() in StopAlarm - one-shot alarm disable
for i in range(len(lines)):
    s = lines[i].strip()
    if s == 'alarms_[i].enabled = false;' or s == 'alarms_[i].enabled = false':
        prev_ctx = ' '.join(lines[max(0,i-5):i])
        if 'one-shot' in prev_ctx.lower() or 'repeat_daily' in prev_ctx:
            lines.insert(i+1, '                SaveAlarmsToNvs();')
            print('Fix 3: Added SaveAlarmsToNvs() in StopAlarm')
            break

# Fix 4: PlayCuckooSoundSync - replace with MixIntoBackgroundAudio
for i in range(len(lines)):
    if 'void BellSoundPlayer::PlayCuckooSoundSync' in lines[i]:
        depth = 0
        func_end = i
        for j in range(i, len(lines)):
            if '{' in lines[j]: depth += lines[j].count('{')
            if '}' in lines[j]:
                depth -= lines[j].count('}')
                if depth == 0:
                    func_end = j
                    break
        new_func = [
            lines[i],
            '    // 使用叠加混音，不中断背景音乐',
            '    auto& app = Application::GetInstance();',
            '    auto& audio = app.GetAudioService();',
            '    if (audio.IsBgAudioActive()) {',
            '        audio.MixIntoBackgroundAudio(cuckoo_wake_sound, CUCKOO_WAKE_SOUND_NUM_SAMPLES, 0.9f);',
            '        vTaskDelay(pdMS_TO_TICKS(1800));',
            '    } else {',
            '        audio.OutputRawPcm(',
            '            cuckoo_wake_sound,',
            '            CUCKOO_WAKE_SOUND_NUM_SAMPLES,',
            '            CUCKOO_WAKE_SOUND_SAMPLE_RATE',
            '        );',
            '    }',
            '}',
        ]
        lines[i:func_end+1] = new_func
        print(f'Fix 4: Replaced PlayCuckooSoundSync')
        break

# Write back as UTF-8 without BOM
out = '\n'.join(lines)
with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'w', encoding='utf-8') as f:
    f.write(out)
print(f'Written {len(lines)} lines')
