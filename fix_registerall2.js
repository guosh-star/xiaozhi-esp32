const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');

// Insert closing brace for RegisterAll before cuckoo_clock_task
// Replace "// 钟控 FreeRTOS 任务" with "}\n\n// 钟控 FreeRTOS 任务"
c = c.replace(
  '// ============================================\n// 钟控 FreeRTOS 任务 (Core 1)\n// ============================================',
  '}\n\n// ============================================\n// 钟控 FreeRTOS 任务 (Core 1)\n// ============================================'
);

// Also remove the extra closing brace we added at end of file
c = c.trimEnd();
// Remove last } if it exists (the one we added in fix_final_brace.js)
if (c.endsWith('}')) {
  c = c.substring(0, c.length - 1).trimEnd();
}

fs.writeFileSync(path, c, 'utf8');
console.log('Fixed RegisterAll closure');

// Verify
const check = fs.readFileSync(path, 'utf8');
const opens = (check.match(/{/g) || []).length;
const closes = (check.match(/}/g) || []).length;
console.log('Braces: open=' + opens + ' close=' + closes + (opens === closes ? ' ✅' : ' ❌'));
