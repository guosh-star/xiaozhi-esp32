const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/sdkconfig';
let c = fs.readFileSync(path, 'utf8');
c = c.replace('CONFIG_CUSTOM_WAKE_WORD_DISPLAY="布谷鸟"', 'CONFIG_CUSTOM_WAKE_WORD_DISPLAY="bu gu niao"');
fs.writeFileSync(path, c, 'utf8');
console.log('Done - replaced 布谷鸟 with bu gu niao');
