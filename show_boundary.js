const fs = require('fs');
const c = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc', 'utf8');
const lines = c.split('\n');

// Show lines around the boundary between RegisterAll end and cuckoo_clock_task start
for (let i = 785; i < 815; i++) {
  console.log((i+1) + ': ' + lines[i]);
}
