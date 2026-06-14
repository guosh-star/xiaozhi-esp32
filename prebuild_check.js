const fs = require('fs');

// 1. 检查 config.h
const config = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/config.h', 'utf8');
console.log('=== config.h 检查 ===');
console.log('DISPLAY_SDA_PIN:', config.includes('DISPLAY_SDA_PIN'));
console.log('DISPLAY_SCL_PIN:', config.includes('DISPLAY_SCL_PIN'));
console.log('AUDIO_I2S_MIC_GPIO_WS:', config.includes('AUDIO_I2S_MIC_GPIO_WS'));
console.log('AUDIO_I2S_SPK_GPIO_BCLK:', config.includes('AUDIO_I2S_SPK_GPIO_BCLK'));

// Check no NC display pins
const displayNC = ['DISPLAY_BACKLIGHT_PIN', 'DISPLAY_MOSI_PIN', 'DISPLAY_SCLK_PIN', 'DISPLAY_DC_PIN', 'DISPLAY_RST_PIN', 'DISPLAY_CS_PIN'];
for (const pin of displayNC) {
  if (config.includes(pin)) {
    console.log(`⚠️  ${pin} 仍存在于配置中`);
  }
}
console.log('✅ 所有显示引脚已配置');

// 2. 检查 cuckoo_board.cc 的关键内容
const board = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_board.cc', 'utf8');
console.log('\n=== cuckoo_board.cc 检查 ===');

const checks = [
  'i2c_master_bus_handle_t',
  'InitializeDisplayI2c',
  'InitializeDisplay',
  'i2c_new_master_bus',
  'esp_lcd_new_panel_io_i2c_v2',
  'esp_lcd_new_panel_ssd1306',
  'esp_lcd_panel_init',
  'OledDisplay',
  '#include <driver/i2c_master.h>',
  '#include <esp_lcd_panel_ops.h>',
  '#include <esp_lcd_panel_vendor.h>',
  'NoDisplay',
];

for (const check of checks) {
  const count = (board.match(new RegExp(check.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'g')) || []).length;
  console.log((count > 0 ? '✅' : '❌') + ` ${check}: ${count}处`);
}

// Check GetDisplay returns display_ not noDisplay
if (board.includes('return display_;') && !board.includes('return &display;')) {
  console.log('✅ GetDisplay 返回 display_');
} else {
  console.log('❌ GetDisplay 可能返回不对');
  const gdIdx = board.indexOf('GetDisplay()');
  const gdBlock = board.substring(gdIdx, gdIdx + 200);
  console.log('  GetDisplay 代码:', gdBlock.substring(0, 100));
}

// 3. 检查大括号
const opens = (board.match(/{/g) || []).length;
const closes = (board.match(/}/g) || []).length;
console.log('\n=== 大括号平衡 ===');
console.log(opens === closes ? `✅ open=${opens} close=${closes}` : `❌ open=${opens} close=${closes}`);

// 4. 检查 sdkconfig 中 WDT 是否已禁用
const sdk = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/sdkconfig', 'utf8');
console.log('\n=== sdkconfig 检查 ===');
console.log('ESP_TASK_WDT_EN:', sdk.includes('CONFIG_ESP_TASK_WDT_EN=y') ? '❌ 仍启用' : '✅ 已禁用');

// 5. 检查引脚冲突
const configLines = config.split('\n');
const pinMap = {};
for (const line of configLines) {
  const m = line.match(/#define\s+(\w+)\s+GPIO_NUM_(\d+)/);
  if (m) {
    const name = m[1];
    const pin = m[2];
    if (!pinMap[pin]) pinMap[pin] = [];
    pinMap[pin].push(name);
  }
}
console.log('\n=== 引脚冲突检查 ===');
let conflictCount = 0;
for (const [pin, names] of Object.entries(pinMap)) {
  if (names.length > 1) {
    console.log(`⚠️  GPIO ${pin} 冲突: ${names.join(', ')}`);
    conflictCount++;
  }
}
if (conflictCount === 0) console.log('✅ 无引脚冲突');
else console.log(`⚠️  发现 ${conflictCount} 个引脚冲突`);

// 6. 检查 CMakeLists.txt 是否有 LCD 依赖
// 先不检查，等build结果

console.log('\n=== 检查完毕 ===');
