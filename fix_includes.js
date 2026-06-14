const fs = require('fs');
const base = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32';

// Fix cuckoo_board.cc includes
let board = fs.readFileSync(base + '/main/boards/cuckoo-clock/cuckoo_board.cc', 'utf8');
board = board
  .replace('../common/codecs/no_audio_codec.h', '../../audio/codecs/no_audio_codec.h')
  .replace('../common/display/oled_display.h', '../../display/oled_display.h')
  .replace('../common/mcp_server.h', '../../mcp_server.h')
  .replace('../common/led/single_led.h', '../../led/single_led.h')
  .replace('../common/assets/lang_config.h', '../../assets/lang_config.h');
fs.writeFileSync(base + '/main/boards/cuckoo-clock/cuckoo_board.cc', board, 'utf8');
console.log('Fixed cuckoo_board.cc');

// Fix cuckoo_controller.cc includes
let ctrl = fs.readFileSync(base + '/main/boards/cuckoo-clock/cuckoo_controller.cc', 'utf8');
ctrl = ctrl.replace('../common/mcp_server.h', '../../mcp_server.h');
fs.writeFileSync(base + '/main/boards/cuckoo-clock/cuckoo_controller.cc', ctrl, 'utf8');
console.log('Fixed cuckoo_controller.cc');

console.log('All includes fixed!');
