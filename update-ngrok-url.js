const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

console.log('🔍 Detecting ngrok tunnel URL...');

try {
  // Start ngrok briefly to capture the URL, then kill it
  const proc = execSync('npx ngrok http 3000 --log=stdout', { 
    timeout: 8000,
    encoding: 'utf-8'
  });
  
  // Extract the HTTPS URL from ngrok logs
  const match = proc.match(/https:\/\/[a-z0-9-]+\.ngrok-free\.app/);
  if (!match) throw new Error('Could not detect ngrok URL');
  
  const TUNNEL_URL = match[0];
  console.log(`✅ Detected Tunnel: ${TUNNEL_URL}`);

  // Update sketch.ino
  const sketchPath = path.join(__dirname, 'sketch.ino');
  let content = fs.readFileSync(sketchPath, 'utf8');
  
  content = content.replace(
    /const char\* kioskApiUrl = ".*?";/g,
    `const char* kioskApiUrl = "${TUNNEL_URL}/api/hardware/event";`
  );
  content = content.replace(
    /const char\* kioskStatusUrl = ".*?";/g,
    `const char* kioskStatusUrl = "${TUNNEL_URL}/api/hardware/status";`
  );
  
  fs.writeFileSync(sketchPath, content);
  console.log('💾 Firmware URLs updated successfully!');
  
} catch (error) {
  console.error(' Failed to update firmware URL:', error.message);
  console.log('⚠️  Please manually update sketch.ino with the current ngrok URL');
}