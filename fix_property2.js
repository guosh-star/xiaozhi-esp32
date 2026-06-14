const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');

// Now fix the 4 PLACEHOLDER_-affected regions
// Replace the broken chunks with proper PropertyList with AddProperty

// Pattern 1: cuckoo.performance with hour
c = c.replace(
    `PLACEHOLDER_
            Property("hour", kPropertyTypeInteger, 1, 12),
        },
        [this](const PropertyList& props) -> ReturnValue {
            int hour = props["hour"].value<int>();
            state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
            return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
        });`,
    `[this](const PropertyList& props) -> ReturnValue {
            int hour = props["hour"].value<int>();
            state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
            return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
        });`);

// Pattern 2: cuckoo.play_music with track  
c = c.replace(
    `PLACEHOLDER_
            Property("track", kPropertyTypeInteger, 1, 12),
        },
        [this](const PropertyList& props) -> ReturnValue {
            int track = props["track"].value<int>();
            state_machine_->PlayMusic(track);
            return std::string("{\"status\": \"playing\", \"track\": " + std::to_string(track) + "}");
        });`,
    `[this](const PropertyList& props) -> ReturnValue {
            int track = props["track"].value<int>();
            state_machine_->PlayMusic(track);
            return std::string("{\"status\": \"playing\", \"track\": " + std::to_string(track) + "}");
        });`);

// Pattern 3: cuckoo.set_servo with servo_id + angle
c = c.replace(
    `PLACEHOLDER_
            Property("servo_id", kPropertyTypeInteger, 0, 1),
            Property("angle", kPropertyTypeInteger, 0, 180),
        },
        [this](const PropertyList& props) -> ReturnValue {
            int id = props["servo_id"].value<int>();
            int angle = props["angle"].value<int>();
            state_machine_->SetServoAngle(id, angle);
            return std::string("{\"status\": \"ok\", \"servo\": " + std::to_string(id) + 
                               ", \"angle\": " + std::to_string(angle) + "}");
        });`,
    `[this](const PropertyList& props) -> ReturnValue {
            int id = props["servo_id"].value<int>();
            int angle = props["angle"].value<int>();
            state_machine_->SetServoAngle(id, angle);
            return std::string("{\"status\": \"ok\", \"servo\": " + std::to_string(id) + 
                               ", \"angle\": " + std::to_string(angle) + "}");
        });`);

// Pattern 4: cuckoo.set_motor with motor_id + speed
c = c.replace(
    `PLACEHOLDER_
            Property("motor_id", kPropertyTypeInteger, 1, 4),
            Property("speed", kPropertyTypeInteger, -100, 100),
        },
        [this](const PropertyList& props) -> ReturnValue {
            int id = props["motor_id"].value<int>();
            int speed = props["speed"].value<int>();
            state_machine_->SetMotorSpeed(id, speed);
            return std::string("{\"status\": \"ok\"}");
        });`,
    `[this](const PropertyList& props) -> ReturnValue {
            int id = props["motor_id"].value<int>();
            int speed = props["speed"].value<int>();
            state_machine_->SetMotorSpeed(id, speed);
            return std::string("{\"status\": \"ok\"}");
        });`);

// Now all 4 PLACEHOLDER blocks should be clean
// But the AddTool calls now all have PropertyList() missing - 
// The lambda became the 3rd argument of AddTool!
// Fix AddTool calls that are missing the PropertyList parameter
// Currently looks like: mcp.AddTool("cuckoo.performance", "desc", [lambda])
// Need to insert PropertyList() as 3rd arg

// Actually wait - looking at the code more carefully:
// The original was:
//   mcp.AddTool("name", "desc", PropertyList(...), [lambda])
// After PLACEHOLDER_ removal, it became:
//   mcp.AddTool("name", "desc", [lambda])
// The lambda is now the 3rd arg directly, but AddTool expects 4 args!
// We need to add PropertyList() between desc and lambda

// But wait - it might still work if the 3rd param is the lambda callback
// Let me check the AddTool signature:
// void AddTool(name, description, properties, callback)
// So we need to insert PropertyList() before the [lambda]
// Add it right before each [
c = c.replace(
    `"Execute a full cuckoo clock performance (door opens, bird comes out, calls, closes, then dance + waterwheel)",
        [this](const PropertyList& props) -> ReturnValue {`,
    `"Execute a full cuckoo clock performance (door opens, bird comes out, calls, closes, then dance + waterwheel)",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {`);

c = c.replace(
    `"Play a background music track from the MP3 module (1-12)",
        [this](const PropertyList& props) -> ReturnValue {`,
    `"Play a background music track from the MP3 module (1-12)",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {`);

c = c.replace(
    `"Set servo angle (0=violinist, 1=dog head shake), angle 0-180",
        [this](const PropertyList& props) -> ReturnValue {`,
    `"Set servo angle (0=violinist, 1=dog head shake), angle 0-180",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {`);

c = c.replace(
    `"Set motor speed (motor_id 1-4, speed -100 to 100, 0=stop)",
        [this](const PropertyList& props) -> ReturnValue {`,
    `"Set motor speed (motor_id 1-4, speed -100 to 100, 0=stop)",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {`);

// Clean up any remaining PLACEHOLDER_
c = c.replace(/PLACEHOLDER_/g, '');

fs.writeFileSync(path, c, 'utf8');
console.log('All 4 AddTool calls fixed');

// Verify
const check = fs.readFileSync(path, 'utf8');
console.log('Remaining PLACEHOLDER:', (check.match(/PLACEHOLDER/g)||[]).length);
console.log('Remaining PropertyList(vector):', (check.match(/PropertyList\(std::vector/g)||[]).length);
