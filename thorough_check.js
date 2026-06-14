const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
const c = fs.readFileSync(path, 'utf8');
const lines = c.split('\n');

console.log('=== 1. 大括号平衡 ===');
let depth = 0;
let badDepth = false;
for (let i = 0; i < lines.length; i++) {
  const l = lines[i];
  const po = (l.match(/{/g) || []).length;
  const pc = (l.match(/}/g) || []).length;
  depth += po - pc;
  if (depth < 0) {
    console.log('  ❌ 第' + (i+1) + '行 depth 变负数！');
    badDepth = true;
  }
}
console.log('  最终 depth:', depth, depth === 0 ? '✅' : '❌');
console.log('  badDepth:', badDepth ? '❌' : '✅');

console.log('\n=== 2. 每个类的函数是否在正确的深度 ===');
depth = 0;
let inFunction = false;
for (let i = 0; i < lines.length; i++) {
  const l = lines[i];
  const po = (l.match(/{/g) || []).length;
  const pc = (l.match(/}/g) || []).length;
  const trimmed = l.trim();
  
  // Detect function definitions at depth 0
  if (depth === 0 && (trimmed.startsWith('void ') || trimmed.startsWith('static ') || 
      trimmed.includes('::') || trimmed.startsWith('Cuckoo') || trimmed.startsWith('Motor') ||
      trimmed.startsWith('Servo') || trimmed.startsWith('BirdJump') || trimmed.startsWith('Mp3Player') ||
      trimmed.startsWith('RtcPcf') || trimmed.startsWith('LdrSensor'))) {
    if (trimmed.endsWith('{') || trimmed.endsWith(') {') || trimmed.indexOf('(') > 0) {
      console.log('  ✅ 函数定义第' + (i+1) + '行: ' + trimmed.substring(0, 80));
    }
  }
  
  depth += po - pc;
}

console.log('\n=== 3. RegisterAll 边界 ===');
const raStart = c.indexOf('void CuckooTools::RegisterAll()');
const raOpen = c.indexOf('{', raStart);
const raClose = c.indexOf('}', raOpen);
// Find the actual closing brace (skip nested braces)
let braceDepth = 1;
let raEnd = -1;
for (let i = raOpen + 1; i < c.length; i++) {
  if (c[i] === '{') braceDepth++;
  if (c[i] === '}') {
    braceDepth--;
    if (braceDepth === 0) { raEnd = i; break; }
  }
}
if (raEnd >= 0) {
  const after = c.substring(raEnd + 1, raEnd + 200);
  console.log('  RegisterAll 结束位置: 第' + c.substring(0, raEnd).split('\n').length + '行');
  console.log('  后面的内容 (前200字符):');
  console.log(after.substring(0, 200));
}

console.log('\n=== 4. Include 路径 ===');
const includes = lines.filter(l => l.trim().startsWith('#include'));
includes.forEach(l => {
  const match = l.match(/#include\s+"([^"]+)"/);
  if (match) {
    const incPath = match[1];
    const fullPath = require('path').resolve(path, '..', incPath);
    const exists = fs.existsSync(fullPath);
    console.log('  ' + (exists ? '✅' : '❌') + ' ' + l.trim() + ' → ' + fullPath);
  } else {
    console.log('  ⚠️ ' + l.trim());
  }
});

console.log('\n=== 5. 检查残留问题 ===');
console.log('  PropertyList(vector):', (c.match(/PropertyList\(std::vector/g)||[]).length, c.match(/PropertyList\(std::vector/g)||[] ? '❌' : '✅');
console.log('  PLACEHOLDER:', (c.match(/PLACEHOLDER/g)||[]).length, (c.match(/PLACEHOLDER/g)||[]).length === 0 ? '✅' : '❌');
console.log('  sm->rtc_:', (c.match(/sm->rtc_/g)||[]).length, (c.match(/sm->rtc_/g)||[]).length === 0 ? '✅' : '❌');
console.log('  LEDC_HIGH_SPEED:', (c.match(/LEDC_HIGH_SPEED_MODE/g)||[]).length, (c.match(/LEDC_HIGH_SPEED_MODE/g)||[]).length === 0 ? '✅' : '❌');
console.log('  AddTool calls:', (c.match(/AddTool/g)||[]).length, (c.match(/AddTool/g)||[]).length === 10 ? '✅' : '⚠️');

console.log('\n=== 6. 文件末尾 ===');
const last100 = lines.slice(-20).join('\n');
console.log(last100);
