const fs = require('fs');
const c = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc', 'utf8');
const lines = c.split('\n');
let depth = 0;
for (let i = 0; i < lines.length; i++) {
  const l = lines[i];
  const po = (l.match(/{/g) || []).length;
  const pc = (l.match(/}/g) || []).length;
  const nd = depth + po - pc;
  const trimmed = l.trim();
  // Show lines where depth changes at top level (depth 0->1 or 1->0)
  if (nd !== depth && (depth <= 1 || nd <= 1)) {
    console.log((i+1) + ': depth ' + depth + ' -> ' + nd + ' | ' + trimmed.substring(0, 120));
  }
  depth = nd;
}
console.log('Final depth:', depth);
