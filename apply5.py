import sys

with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'r', encoding='utf-8') as f:
    lines = f.readlines()

print(f'Lines: {len(lines)}')

# Fix 1: SaveHourlyPerf/LoadHourlyPerf after LoadKidsActive
for i in range(len(lines)):
    if 'void CuckooStateMachine::LoadKidsActive() {' in lines[i]:
        depth = 0
        for j in range(i, len(lines)):
            depth += lines[j].count('{')
            depth -= lines[j].count('}')
            if depth == 0:
                insert = [
                    '\n',
                    'void CuckooStateMachine::SaveHourlyPerf() {\n',
                    '    nvs_handle_t nvs;\n',
                    '    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {\n',
                    '        nvs_set_i8(nvs, "hperf", (int8_t)hourly_perf_.load());\n',
                    '        nvs_commit(nvs);\n',
                    '        nvs_close(nvs);\n',
                    '    }\n',
                    '}\n',
                    '\n',
                    'void CuckooStateMachine::LoadHourlyPerf() {\n',
                    '    nvs_handle_t nvs;\n',
                    '    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {\n',
                    '        int8_t val;\n',
                    '        if (nvs_get_i8(nvs, "hperf", &val) == ESP_OK) hourly_perf_ = (bool)val;\n',
                    '        nvs_close(nvs);\n',
                    '    }\n',
                    '}\n',
                ]
                lines[j+1:j+1] = insert
                print(f'Fix 1 OK')
                break
        break

# Fix 2: LoadHourlyPerf() call
for i in range(len(lines)):
    if 'sm->LoadKidsActive()' in lines[i]:
        lines.insert(i+1, '    sm->LoadHourlyPerf();\n')
        print('Fix 2 OK')
        break

# Fix 3: SaveAlarmsToNvs in StopAlarm
for i in range(len(lines)):
    s = lines[i].strip()
    if s == 'alarms_[i].enabled = false;' or s == 'alarms_[i].enabled = false':
        prev = ''.join(lines[max(0,i-5):i]).lower()
        if 'one-shot' in prev or 'repeat_daily' in prev:
            lines.insert(i+1, '                SaveAlarmsToNvs();\n')
            print('Fix 3 OK')
            break

# Fix 4: PlayCuckooSoundSync
for i in range(len(lines)):
    if 'void BellSoundPlayer::PlayCuckooSoundSync()' in lines[i]:
        depth = 0
        for j in range(i, len(lines)):
            depth += lines[j].count('{')
            depth -= lines[j].count('}')
            if depth == 0:
                new_body = [
                    '{\n',
                    '    // Use MixIntoBackgroundAudio to avoid interrupting music\n',
                    '    auto& app = Application::GetInstance();\n',
                    '    auto& audio = app.GetAudioService();\n',
                    '    if (audio.IsBgAudioActive()) {\n',
                    '        audio.MixIntoBackgroundAudio(cuckoo_wake_sound, CUCKOO_WAKE_SOUND_NUM_SAMPLES, 0.9f);\n',
                    '        vTaskDelay(pdMS_TO_TICKS(1800));\n',
                    '    } else {\n',
                    '        audio.OutputRawPcm(\n',
                    '            cuckoo_wake_sound,\n',
                    '            CUCKOO_WAKE_SOUND_NUM_SAMPLES,\n',
                    '            CUCKOO_WAKE_SOUND_SAMPLE_RATE\n',
                    '        );\n',
                    '    }\n',
                    '}\n',
                ]
                lines[i+1:j+1] = new_body
                print('Fix 4 OK')
                break
        break

# Fix 5: MCP descriptions
for i in range(len(lines)):
    line = lines[i]
    if 'Dog show: call when user asks about dog/puppy/Lisa.' in line:
        lines[i] = line.replace(
            'Dog show: call when user asks about dog/puppy/Lisa. Keep response very brief - one short sentence only.',
            'Wake words: Lisa where are you, Lisa Lisa, Lisa come out. Dog show. Reply with one short sentence.'
        )
        print('Fix 5a OK')
    if 'Linda show: call when user asks about Linda or dancing.' in line:
        lines[i] = line.replace(
            'Linda show: call when user asks about Linda or dancing.',
            'Wake words: Linda Linda, Linda where are you, Linda come out and dance. Linda show. Reply with one short sentence.'
        )
        print('Fix 5b OK')
    if 'Garden show: call when user asks about Garden//violin.' in line:
        lines[i] = line.replace(
            'Garden show: call when user asks about Garden//violin.',
            'Wake words: Yuanzi Yuanzi, Yuanzi where are you, Yuanzi play guitar. Garden show. Reply with one short sentence.'
        )
        print('Fix 5c OK')

# Write back
with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'w', encoding='utf-8') as f:
    f.writelines(lines)
print(f'Written {len(lines)} lines')
