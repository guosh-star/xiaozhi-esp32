const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');

// Replace the LdrSensor constructor
c = c.replace(
`LdrSensor::LdrSensor(gpio_num_t adc_pin, int threshold)
    : adc_pin_(adc_pin), threshold_(threshold) {
    
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
    };
    adc_oneshot_new_unit(&init_cfg, NULL);  // 简化处理，实际应检查返回
}`,
`LdrSensor::LdrSensor(gpio_num_t adc_pin, int threshold)
    : adc_pin_(adc_pin), threshold_(threshold) {
    // LDR sensor: ADC reading not fully configured yet
    ESP_LOGI(TAG, "LDR sensor on GPIO %d, threshold %d", adc_pin_, threshold_);
}`);

// Replace LdrSensor::ReadRaw
c = c.replace(
`int LdrSensor::ReadRaw() {
    int value = 0;
    adc_oneshot_read(NULL, adc_channel_, &value);  // 简化
    return value;
}`,
`int LdrSensor::ReadRaw() {
    // Placeholder: use GPIO level as simple light indicator
    int level = gpio_get_level(adc_pin_);
    return level == 1 ? 4095 : 0;
}`);

// Add missing include for ESP_LOGI in the file if not already included
if (!c.includes('#include <esp_log.h>')) {
  c = c.replace('#include <cmath>', '#include <cmath>\n#include <esp_log.h>');
}

fs.writeFileSync(path, c, 'utf8');
console.log('ADC fixes applied');

// Verify
const idx=c.indexOf('adc_oneshot');
console.log('adc_oneshot remaining:', idx >= 0 ? 'YES' : 'NO');
