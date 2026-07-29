import sys

with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Find LoadKidsActive and its closing brace
found = False
for i in range(len(lines)):
    if 'void CuckooStateMachine::LoadKidsActive()' in lines[i]:
        depth = 0
        for j in range(i, len(lines)):
            if '{' in lines[j]:
                depth += 1
            if '}' in lines[j]:
                depth -= 1
                if depth == 0:
                    insert_at = j + 1
                    new_code = [
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
                    lines[insert_at:insert_at] = new_code
                    print(f'Added SaveHourlyPerf/LoadHourlyPerf after line {j+1}')
                    found = True
                    break
        break

if found:
    with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print(f'Done, {len(lines)} lines total')
else:
    print('LoadKidsActive not found!')
