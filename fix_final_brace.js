const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');

// Append a closing brace at the end of the file
c = c.trimEnd() + '\n}\n';

fs.writeFileSync(path, c, 'utf8');
console.log('Added closing brace at end of file');

// Verify
const check = fs.readFileSync(path, 'utf8');
const opens = (check.match(/{/g) || []).length;
const closes = (check.match(/}/g) || []).length;
console.log('Braces: open=' + opens + ' close=' + closes + (opens === closes ? ' ✅' : ' ❌'));
