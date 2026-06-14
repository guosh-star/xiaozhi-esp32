const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/sdkconfig';
let c = fs.readFileSync(path, 'utf8');
// Replace the wake word with no spaces to be safe
c = c.replace('CONFIG_CUSTOM_WAKE_WORD="bu gu niao"', 'CONFIG_CUSTOM_WAKE_WORD="cuckoo"');
fs.writeFileSync(path, c, 'utf8');
console.log('Done - replaced bu gu niao with cuckoo');
