import os, sys

# Step 1: Get clean version via git, write with UTF-8 CRLF
os.system('git show 7c654dc:main/boards/cuckoo-clock/cuckoo_controller.cc > _clean.cc')
with open('_clean.cc', 'rb') as f:
    raw = f.read()
os.remove('_clean.cc')

if raw[:2] == b'\xff\xfe':
    text = raw[2:].decode('utf-16-le')
elif raw[:3] == b'\xef\xbb\xbf':
    text = raw[3:].decode('utf-8')
else:
    text = raw.decode('utf-8')

# Normalize: replace ALL line endings with \n, then split
text = text.replace('\r\n', '\n').replace('\r', '\n')
lines = text.split('\n')

print(f'Lines: {len(lines)}')

# === Fix 1: SaveHourlyPerf/LoadHourlyPerf ===
for i in range(len(lines)):
    if 'void CuckooStateMachine::LoadKidsActive()' in lines[i]:
        depth = 0
        for j in range(i, len(lines)):
            depth += lines[j].count('{')
            depth -= lines[j].count('}')
            if depth == 0:
                new_lines = [
                    '',
                    'void CuckooStateMachine::SaveHourlyPerf() {',
                    '    nvs_handle_t nvs;',
                    '    if (nvs_open("cuckoo", NVS_READWRITE, &nvs) == ESP_OK) {',
                    '        nvs_set_i8(nvs, "hperf", (int8_t)hourly_perf_.load());',
                    '        nvs_commit(nvs);',
                    '        nvs_close(nvs);',
                    '    }',
                    '}',
                    '',
                    'void CuckooStateMachine::LoadHourlyPerf() {',
                    '    nvs_handle_t nvs;',
                    '    if (nvs_open("cuckoo", NVS_READONLY, &nvs) == ESP_OK) {',
                    '        int8_t val;',
                    '        if (nvs_get_i8(nvs, "hperf", &val) == ESP_OK) hourly_perf_ = (bool)val;',
                    '        nvs_close(nvs);',
                    '    }',
                    '}',
                ]
                lines[j+1:j+1] = new_lines
                print('Fix 1: OK')
                break
        break

# === Fix 2: LoadHourlyPerf call ===
for i in range(len(lines)):
    if 'sm->LoadKidsActive()' in lines[i]:
        lines.insert(i+1, '    sm->LoadHourlyPerf();')
        print('Fix 2: OK')
        break

# === Fix 3: SaveAlarmsToNvs in StopAlarm ===
for i in range(len(lines)):
    stripped = lines[i].strip()
    if stripped == 'alarms_[i].enabled = false;' or stripped == 'alarms_[i].enabled = false':
        prev = ' '.join(lines[max(0,i-5):i]).lower()
        if 'one-shot' in prev or 'repeat_daily' in prev:
            lines.insert(i+1, '                SaveAlarmsToNvs();')
            print('Fix 3: OK')
            break

# === Fix 4: PlayCuckooSoundSync ===
for i in range(len(lines)):
    if 'void BellSoundPlayer::PlayCuckooSoundSync' in lines[i]:
        depth = 0
        for j in range(i, len(lines)):
            depth += lines[j].count('{')
            depth -= lines[j].count('}')
            if depth == 0:
                lines[i+1:j+1] = [
                    '    // Use MixIntoBackgroundAudio to avoid interrupting music',
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
                ]
                # Need closing brace after our new code
                lines.insert(i + len(lines[i+1:j+1]) + 1, '}')
                print('Fix 4: OK')
                break
        break

# === Fix 5: MCP descriptions (using ASCII quotes only, no Chinese " ") ===
mcp_fixes = [
    ('Dog show: call when user asks about dog/puppy/Lisa. Keep response very brief - one short sentence only.',
     'Wake words: Lisa where are you, Lisa Lisa, Lisa come out. Dog show: open door, dog runs out, wags tail, barks twice, goes back. Reply with one short sentence.'),
    ('Linda show: call when user asks about Linda or dancing.',
     'Wake words: Linda Linda, Linda where are you, Linda come out and dance. Linda show: music + dance motor + LED. Reply with one short sentence.'),
    ('Garden show: call when user asks about Garden//violin.',
     'Wake words: Yuanzi Yuanzi, Yuanzi where are you, Yuanzi play guitar. Garden show: music + violin servo + LED. Reply with one short sentence.'),
]
for i in range(len(lines)):
    for old, new in mcp_fixes:
        if old in lines[i]:
            lines[i] = lines[i].replace(old, new)
            print(f'Fix 5: Updated {old[:40]}...')
            mcp_fixes.remove((old, new))
            break

# Write with CRLF
out_path = 'main/boards/cuckoo-clock/cuckoo_controller.cc'
with open(out_path, 'wb') as f:
    content = '\r\n'.join(lines)
    f.write(content.encode('utf-8'))
print(f'Written {len(lines)} lines, {len(content)} bytes')
