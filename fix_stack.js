const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/sdkconfig';
let s = fs.readFileSync(path, 'utf8');

// Increase sys_evt task stack from 2304 to 4096
s = s.replace(
  'CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304',
  'CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096'
);
s = s.replace(
  'CONFIG_SYSTEM_EVENT_TASK_STACK_SIZE=2304',
  'CONFIG_SYSTEM_EVENT_TASK_STACK_SIZE=4096'
);

fs.writeFileSync(path, s, 'utf8');
console.log('sys_evt stack size increased to 4096');

// Verify
s = fs.readFileSync(path, 'utf8');
s.split('\n').filter(l => l.match(/SYSTEM_EVENT.*STACK_SIZE|STACK_SIZE.*4096/)).forEach(l => console.log(l));
