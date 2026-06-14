const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');
c = c.replace('(is_dark_ && hour >= 20) || hour < 7 {', '(is_dark_ && hour >= 20) || hour < 7) {');
fs.writeFileSync(path, c, 'utf8');
console.log('Fixed bracket');
