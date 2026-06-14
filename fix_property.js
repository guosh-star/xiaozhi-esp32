const fs = require('fs');
const path = 'C:/Users/GUO-notebook/.openclaw/workspace/xiaozhi-esp32/main/boards/cuckoo-clock/cuckoo_controller.cc';
let c = fs.readFileSync(path, 'utf8');

// Replace "PropertyList(std::vector<Property>{ ... }," with "PropertyList(),"
// For simple singletons: Property("xxx", type, min, max)
// For doubles: Property("xxx", type, min, max), Property("yyy", ...)

// Pattern 1: hour (single)
c = c.replace(
  /PropertyList\(std::vector<Property>\{\s*Property\("hour",\s*kPropertyTypeInteger,\s*1,\s*12\)\s*\},/g,
  'PropertyList(), /* hour */'
);

// Pattern 2: track (single)
c = c.replace(
  /PropertyList\(std::vector<Property>\{\s*Property\("track",\s*kPropertyTypeInteger,\s*1,\s*12\)\s*\},/g,
  'PropertyList(), /* track */'
);

// Pattern 3: servo_id + angle (double)
c = c.replace(
  /PropertyList\(std::vector<Property>\{\s*Property\("servo_id",\s*kPropertyTypeInteger,\s*0,\s*1\),\s*Property\("angle",\s*kPropertyTypeInteger,\s*0,\s*180\)\s*\},/g,
  'PropertyList(), /* servo_id,angle */'
);

// Pattern 4: motor_id + speed (double)
c = c.replace(
  /PropertyList\(std::vector<Property>\{\s*Property\("motor_id",\s*kPropertyTypeInteger,\s*1,\s*4\),\s*Property\("speed",\s*kPropertyTypeInteger,\s*-100,\s*100\)\s*\},/g,
  'PropertyList(), /* motor_id,speed */'
);

console.log('Remaining PropertyList(vector):', (c.match(/PropertyList\(std::vector<Property>/g)||[]).length);

fs.writeFileSync(path, c, 'utf8');
console.log('Done');
