const fs = require('fs');
const { execSync } = require('child_process');

async function getTunnelUrl() {
  // Start localtunnel and capture the URL
  return new Promise((resolve, reject) => {
    const lt = require('localtunnel');
    
    lt({ port: 3000 }, (err, tunnel) => {
      if (err) {
        reject(err);
        return;
      }
      console.log(`Tunnel URL: ${tunnel.url}`);
      resolve(tunnel.url);
    });
  });
}

async function updateFirmwareUrl() {
  try {
    console.log('Getting tunnel URL...');
    const tunnelUrl = await getTunnelUrl();
    const apiUrl = `${tunnelUrl}/api/hardware/event`;
    
    console.log(`Updating sketch.ino with URL: ${apiUrl}`);
    
    // Read sketch.ino
    let sketchContent = fs.readFileSync('sketch.ino', 'utf8');
    
    // Replace the kioskApiUrl line
    const urlRegex = /const char\* kioskApiUrl = ".*?";/;
    const newLine = `const char* kioskApiUrl = "${apiUrl}";`;
    sketchContent = sketchContent.replace(urlRegex, newLine);
    
    // Write back
    fs.writeFileSync('sketch.ino', sketchContent);
    
    console.log('✅ Firmware URL updated successfully!');
    console.log('Now rebuild firmware with: platformio run');
    
  } catch (error) {
    console.error('❌ Error updating firmware URL:', error.message);
  }
}

// Only run if called directly
if (require.main === module) {
  updateFirmwareUrl();
}

module.exports = { updateFirmwareUrl };