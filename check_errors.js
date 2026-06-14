const fs = require('fs');
// Check build log for ALL actual errors
const log = fs.readFileSync('C:/Users/GUO-notebook/.openclaw/media/inbound/build_log---b503e1cd-12b9-4512-95de-dcaa370c0a0d.txt', 'utf8');
const lines = log.split('\n');
// Find ALL error: patterns
lines.forEach((l, i) => {
  if (l.includes('error:') || l.includes('Error:')) {
    console.log((i+1) + ': ' + l.trim());
  }
});
// Also find all cuckoo-related lines
console.log('\n=== cuckoo related ===');
lines.forEach((l, i) => {
  if (l.toLowerCase().includes('cuckoo') || l.includes('cuckoo_controller')) {
    console.log((i+1) + ': ' + l.trim());
  }
});
