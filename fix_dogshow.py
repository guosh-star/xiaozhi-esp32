fname = r'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\main\boards\cuckoo-clock\cuckoo_part4.cc'

with open(fname, 'rb') as f:
    data = f.read()

content = data.decode('utf-8', errors='replace')

# The file has \r\n\r\n (Windows CRLF with blank line between)
# Normalize to single \r\n per line
while '\r\n\r\n' in content:
    # This is two consecutive CRLF = blank line + next line
    pass  # Don't remove blank lines

# Match the exact format from the debug output
old = '    MotorPowerOn();\r\n\r\n    // 1. Water wheel on\r\n    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);\r\n\r\n    // 2. Open door\r\n    if (m2_) {\r\n        m2_->Forward(MAIN_DOOR_OPEN_SPEED);\r\n        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));\r\n        m2_->Stop();\r\n    }\r\n\r\n    // 3. Dog tail out: 180->20, 1200ms'

new = '    MotorPowerOn();\r\n\r\n    // 1. Open door\r\n    if (m2_) {\r\n        m2_->Forward(MAIN_DOOR_OPEN_SPEED);\r\n        vTaskDelay(pdMS_TO_TICKS(MAIN_DOOR_TIME_MS));\r\n        m2_->Stop();\r\n    }\r\n\r\n    // 2. Water wheel on (after door, avoids voltage sag from shared power rail)\r\n    if (water_bird_) water_bird_->SetSpeed(WATER_WHEEL_SPEED);\r\n\r\n    // 3. Dog tail out: 180->20, 1200ms'

if old in content:
    content = content.replace(old, new)
    print('Replaced successfully')
else:
    print('ERROR: old text not found')
    # Debug: show what's actually there
    idx = content.find('MotorPowerOn')
    if idx >= 0:
        snippet = content[idx:idx+450]
        for i, ch in enumerate(snippet):
            if ch == '\r':
                print(f'  \\r at pos {idx+i}')
            elif ch == '\n':
                print(f'  \\n at pos {idx+i}')

# Preserve original line endings exactly
with open(fname, 'wb') as f:
    f.write(content.encode('utf-8'))
print('File written')
