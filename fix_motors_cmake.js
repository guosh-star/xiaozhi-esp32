const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_board.cc';
let c = fs.readFileSync(path, 'utf8');

// 1. Add BOARD_TYPE_HELPER_LCD define for CONFIG_* check
// Actually we need to add esp_lcd to PRIV_REQUIRES in CMakeLists.txt
console.log('Need to add esp_lcd to CMakeLists.txt');

// 2. Check: does the constructor call InitializeDisplayI2c + InitializeDisplay?
let hasDisplayInit = c.includes('InitializeDisplayI2c()') && c.includes('InitializeDisplay()');
console.log('Has display init calls:', hasDisplayInit);

// The constructor currently calls:
// InitializeDisplayI2c();
// InitializeDisplay();
// InitializePeripherals();
// This is good - display will be initialized before motor init
// But motor init on pins 41/42 will conflict with I2C bus

// Actually, ESP_ERROR_CHECK(i2c_new_master_bus) will succeed even if pins 41/42 have 
// other GPIO configurations - the I2C bus driver reconfigures the pins.
// BUT subsequent InitializePeripherals() will create Motor objects that call 
// gpio_set_direction on the same pins, which could cause issues.

// For now, the user said "don't worry about peripherals" - but we still create motors
// Solution: comment out motor creation in InitializePeripherals

// Actually a simpler approach: just don't create motors until the user adds the hardware
// Let's wrap motor creation in a check that's off by default

c = c.replace(
`        // 创建电机对象
        m1_ = new Motor(TB6612_1_PWMA, TB6612_1_AIN1, TB6612_1_AIN2, LEDC_CHANNEL_0);
        m2_ = new Motor(TB6612_1_PWMB, TB6612_1_BIN1, TB6612_1_BIN2, LEDC_CHANNEL_1);
        m3_ = new Motor(TB6612_2_PWMA, TB6612_2_AIN1, TB6612_2_AIN2, LEDC_CHANNEL_2);
        m4_ = new Motor(TB6612_2_PWMB, TB6612_2_BIN1, TB6612_2_BIN2, LEDC_CHANNEL_3);

        // 鸟门电机 (使用TB6612_1的PWMA通道空闲... 实际用L9110S或三极管)
        bird_door_motor_ = new Motor(BIRD_DOOR_PWM, BIRD_DOOR_DIR1, BIRD_DOOR_DIR2, LEDC_CHANNEL_6);

        // 舵机
        violin_servo_ = new Servo(SERVO_VIOLIN, LEDC_CHANNEL_4);
        dog_servo_ = new Servo(SERVO_DOG, LEDC_CHANNEL_5);

        // 电磁铁
        bird_jump_ = new BirdJump(BIRD_JUMP_GPIO);`,
`        // 电机/舵机/电磁铁暂时禁用（等待布谷鸟钟外设接入）
        ESP_LOGW(TAG, "Motors/servos disabled - display shares GPIO 41/42");
        // m1_ = new Motor(TB6612_1_PWMA, TB6612_1_AIN1, TB6612_1_AIN2, LEDC_CHANNEL_0);
        // m2_ = new Motor(TB6612_1_PWMB, TB6612_1_BIN1, TB6612_1_BIN2, LEDC_CHANNEL_1);
        // m3_ = new Motor(TB6612_2_PWMA, TB6612_2_AIN1, TB6612_2_AIN2, LEDC_CHANNEL_2);
        // m4_ = new Motor(TB6612_2_PWMB, TB6612_2_BIN1, TB6612_2_BIN2, LEDC_CHANNEL_3);
        // bird_door_motor_ = new Motor(BIRD_DOOR_PWM, BIRD_DOOR_DIR1, BIRD_DOOR_DIR2, LEDC_CHANNEL_6);
        // violin_servo_ = new Servo(SERVO_VIOLIN, LEDC_CHANNEL_4);
        // dog_servo_ = new Servo(SERVO_DOG, LEDC_CHANNEL_5);
        // bird_jump_ = new BirdJump(BIRD_JUMP_GPIO);`
);

fs.writeFileSync(path, c, 'utf8');
console.log('Motors disabled in InitializePeripherals');

// 3. Add esp_lcd to CMakeLists.txt PRIV_REQUIRES for cuckoo-clock
const cmakePath = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/CMakeLists.txt';
let cmake = fs.readFileSync(cmakePath, 'utf8');

// Find the cuckoo-clock section in PRIV_REQUIRES
const searchStr = 'if(CONFIG_BOARD_TYPE_CUCKOO_CLOCK)\n    list(APPEND MAIN_PRIV_REQUIRES_EXTRA\n        esp_driver_ledc\n        esp_adc\n    )\nendif()';
if (cmake.includes(searchStr)) {
  cmake = cmake.replace(
    'if(CONFIG_BOARD_TYPE_CUCKOO_CLOCK)\n    list(APPEND MAIN_PRIV_REQUIRES_EXTRA\n        esp_driver_ledc\n        esp_adc\n    )\nendif()',
    'if(CONFIG_BOARD_TYPE_CUCKOO_CLOCK)\n    list(APPEND MAIN_PRIV_REQUIRES_EXTRA\n        esp_driver_ledc\n        esp_adc\n        esp_lcd\n    )\nendif()'
  );
  fs.writeFileSync(cmakePath, cmake, 'utf8');
  console.log('Added esp_lcd to CMakeLists.txt PRIV_REQUIRES');
} else {
  console.log('WARNING: Could not find cuckoo-clock section in CMakeLists.txt');
}
