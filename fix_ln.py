with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'rb') as f:
    data = f.read()

# The 4 buggy \n are between these boundaries:
# 1-3: After Stop(); } through before Motor::Motor constructor comments
# 4: Before Motor::Motor constructor declaration

# Find the exact regions and replace \n with \r\n (real line break)
# Pattern 1: );\r\n}\n\n// ====== (pos 6276-6278 area)
old = b');\r\n}\x5Cn\x5Cn// ============================================\x5Cn// Motor \xe6\x9e\x84\xe9\x80\xa0\xe5\x87\xbd\xe6\x95\xb0 - PWM'
new = b');\r\n}\r\n\r\n// ============================================\r\n// Motor \xe6\x9e\x84\xe9\x80\xa0\xe5\x87\xbd\xe6\x95\xb0 - PWM'
count1 = data.count(old)
data = data.replace(old, new)

# Pattern 2: // ===\nMotor::Motor( (pos 6530)
old2 = b'// \xe7\x94\xa8\xe9\x80\x94\xef\xbc\x9a\xe5\xb0\x8f\xe6\x8f\x90\xe7\x90\xb4\xe5\x8d\x87\xe9\x99\x8d\xe7\x94\xb5\xe6\x9c\xba\xe3\x80\x81\xe6\xb0\xb4\xe8\xbd\xa6\xe7\x94\xb5\xe6\x9c\xba\r\n// ============================================\x5CnMotor::Motor(gpio_num_t in1'
new2 = b'// \xe7\x94\xa8\xe9\x80\x94\xef\xbc\x9a\xe5\xb0\x8f\xe6\x8f\x90\xe7\x90\xb4\xe5\x8d\x87\xe9\x99\x8d\xe7\x94\xb5\xe6\x9c\xba\xe3\x80\x81\xe6\xb0\xb4\xe8\xbd\xa6\xe7\x94\xb5\xe6\x9c\xba\r\n// ============================================\r\nMotor::Motor(gpio_num_t in1'
count2 = data.count(old2)
data = data.replace(old2, new2)

print(f'Fix 1: {count1} replacements')
print(f'Fix 2: {count2} replacements')

with open('main/boards/cuckoo-clock/cuckoo_controller.cc', 'wb') as f:
    f.write(data)
print('Done')
