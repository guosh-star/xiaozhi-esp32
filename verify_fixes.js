const fs = require('fs');
const c = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc', 'utf8');

// Check 1: No PropertyList(vector) pattern
const pv = (c.match(/PropertyList\(std::vector<Property>/g) || []).length;
console.log('1. PropertyList(vector): ' + pv + (pv === 0 ? ' ✅' : ' ❌'));

// Check 2: No PLACEHOLDER
const ph = (c.match(/PLACEHOLDER/g) || []).length;
console.log('2. PLACEHOLDER: ' + ph + (ph === 0 ? ' ✅' : ' ❌'));

// Check 3: No sm->rtc_
const rtc = (c.match(/sm->rtc_/g) || []).length;
console.log('3. sm->rtc_: ' + rtc + (rtc === 0 ? ' ✅' : ' ❌'));

// Check 4: No LEDC_HIGH_SPEED_MODE
const high = (c.match(/LEDC_HIGH_SPEED_MODE/g) || []).length;
console.log('4. LEDC_HIGH_SPEED_MODE: ' + high + (high === 0 ? ' ✅' : ' ❌'));

// Check 5: No std::vector in PropertyList construct
const sv = (c.match(/std::vector<Property>/g) || []).length;
console.log('5. std::vector<Property>: ' + sv + (sv === 0 ? ' ✅' : ' ❌'));

// Check 6: AddTool calls count (should be 10)
const at = (c.match(/AddTool\(/g) || []).length;
console.log('6. AddTool() calls: ' + at + (at === 10 ? ' ✅' : ' ⚠️ expected 10'));

// Check 7: Includes are correct
console.log('7. Includes:');
const includeLines = c.split('\n').filter(l => l.includes('#include'));
includeLines.forEach(l => console.log('   ' + l.trim()));

// Check 8: Brace balance for RegisterAll function
const start = c.indexOf('void CuckooTools::RegisterAll()');
const end = c.indexOf('ESP_LOGI(TAG, "Cuckoo clock MCP tools registered")');
const funcBody = c.substring(c.indexOf('{', start), c.indexOf('}', end) + 1);
const opens = (funcBody.match(/\{/g) || []).length;
const closes = (funcBody.match(/\}/g) || []).length;
console.log('8. RegisterAll braces: open=' + opens + ' close=' + closes + (opens === closes ? ' ✅' : ' ❌'));

// Check 9: Whole file brace balance
const fopens = (c.match(/\{/g) || []).length;
const fcloses = (c.match(/\}/g) || []).length;
console.log('9. Full file braces: open=' + fopens + ' close=' + fcloses + (fopens === fcloses ? ' ✅' : ' ❌'));
