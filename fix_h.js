const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.h';
let h = fs.readFileSync(path, 'utf8');

// Replace old ADC include with GPIO include
h = h.replace('#include <driver/adc.h>', '#include <driver/gpio.h>');

// Remove adc_channel_t member since we don't use ADC anymore
// Change 'adc_channel_t adc_channel_;' to just a comment or remove
h = h.replace('    adc_channel_t adc_channel_;', '    // adc_channel_ removed - using GPIO level directly');

fs.writeFileSync(path, h, 'utf8');
console.log('Header fixed');
