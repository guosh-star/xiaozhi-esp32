const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');

// Find RegisterAll function boundaries
const start = c.indexOf('void CuckooTools::RegisterAll()');
const endMarker = 'ESP_LOGI(TAG, "Cuckoo clock MCP tools registered")';
const end = c.indexOf(endMarker);
const funcStart = c.indexOf('{', start);
const funcEnd = c.indexOf('}', end) + 1;

console.log('RegisterAll function body:', funcStart, '-', funcEnd);

const body = 
`    auto& mcp = McpServer::GetInstance();

    // 1. cuckoo.performance
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.performance", 
            "Execute a full cuckoo clock performance (door opens, bird comes out, calls, closes, then dance + waterwheel)",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int hour = props["hour"].value<int>();
                state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
                return std::string("{\\"status\\": \\"started\\", \\"hour\\": " + std::to_string(hour) + "}");
            });
    }

    // 2. 纯跳舞
    mcp.AddTool("cuckoo.dance",
        "Make the cuckoo clock's violinist dancer perform a dance (M1+M2 + violin servo)",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->Dance();
            return std::string("{\\"status\\": \\"dancing\\"}");
        });

    // 3. 播放背景音乐
    {
        PropertyList pl;
        pl.AddProperty(Property("track", kPropertyTypeInteger, 1, 12));
        mcp.AddTool("cuckoo.play_music",
            "Play a background music track from the MP3 module (1-12)",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int track = props["track"].value<int>();
                state_machine_->PlayMusic(track);
                return std::string("{\\"status\\": \\"playing\\", \\"track\\": " + std::to_string(track) + "}");
            });
    }

    // 4. 紧急停止
    mcp.AddTool("cuckoo.stop_all",
        "Emergency stop all motors, servos, and MP3 playback immediately",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAll();
            return std::string("{\\"status\\": \\"stopped\\"}");
        });

    // 5. 开鸟门
    mcp.AddTool("cuckoo.open_door",
        "Open the bird door",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->OpenDoor();
            return std::string("{\\"status\\": \\"door opened\\"}");
        });

    // 6. 关鸟门
    mcp.AddTool("cuckoo.close_door",
        "Close the bird door",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->CloseDoor();
            return std::string("{\\"status\\": \\"door closed\\"}");
        });

    // 7. 小鸟跳跃
    mcp.AddTool("cuckoo.bird_jump",
        "Make the bird jump once (electromagnet pulse)",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->BirdJumpOnce();
            return std::string("{\\"status\\": \\"bird jumped\\"}");
        });

    // 8. 舵机控制
    {
        PropertyList pl;
        pl.AddProperty(Property("servo_id", kPropertyTypeInteger, 0, 1));
        pl.AddProperty(Property("angle", kPropertyTypeInteger, 0, 180));
        mcp.AddTool("cuckoo.set_servo",
            "Set servo angle (0=violinist, 1=dog head shake), angle 0-180",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int id = props["servo_id"].value<int>();
                int angle = props["angle"].value<int>();
                state_machine_->SetServoAngle(id, angle);
                return std::string("{\\"status\\": \\"ok\\", \\"servo\\": " + std::to_string(id) + 
                                   ", \\"angle\\": " + std::to_string(angle) + "}");
            });
    }

    // 9. 电机控制
    {
        PropertyList pl;
        pl.AddProperty(Property("motor_id", kPropertyTypeInteger, 1, 4));
        pl.AddProperty(Property("speed", kPropertyTypeInteger, -100, 100));
        mcp.AddTool("cuckoo.set_motor",
            "Set motor speed (motor_id 1-4, speed -100 to 100, 0=stop)",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int id = props["motor_id"].value<int>();
                int speed = props["speed"].value<int>();
                state_machine_->SetMotorSpeed(id, speed);
                return std::string("{\\"status\\": \\"ok\\"}");
            });
    }

    // 10. 获取当前状态
    mcp.AddTool("cuckoo.get_status",
        "Get the current cuckoo clock status (running/idle, time, dark mode)",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            char json[128];
            snprintf(json, sizeof(json), 
                "{\\"running\\": %s, \\"dark_mode\\": %s}",
                state_machine_->IsRunning() ? "true" : "false",
                "false");
            return std::string(json);
        });

`;

const newC = c.substring(0, funcStart + 1) + '\n' + body + c.substring(funcEnd);
fs.writeFileSync(path, newC, 'utf8');
console.log('RegisterAll function fully replaced');
console.log('New file length:', newC.length, 'bytes');

// Quick verify
const check = require('fs').readFileSync(path, 'utf8');
console.log('AddTool calls:', (check.match(/AddTool/g)||[]).length);
console.log('PropertyList():  ', (check.match(/PropertyList\(\)/g)||[]).length);
console.log('PLACEHOLDER remaining:', (check.match(/PLACEHOLDER/g)||[]).length);
console.log('PropertyList(std::vector remaining:', (check.match(/PropertyList\(std::vector/g)||[]).length);
console.log('sm->rtc_ remaining:', (check.match(/sm->rtc_/g)||[]).length);
