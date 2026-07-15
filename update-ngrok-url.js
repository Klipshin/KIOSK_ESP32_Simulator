/**
 * update-ngrok-url.js
 * 
 * Reads CLOUDFLARE_TUNNEL_URL from .env and patches sketch.ino
 * with the correct API endpoint URLs for the ESP32 firmware.
 * 
 * Usage:
 *   1. Update CLOUDFLARE_TUNNEL_URL in .env with your new tunnel URL
 *   2. Run: node update-ngrok-url.js
 *   3. Rebuild firmware: platformio run
 */

require('dotenv').config();
const fs = require('fs');
const path = require('path');

async function main() {
  const TUNNEL_URL = process.env.CLOUDFLARE_TUNNEL_URL;

  if (!TUNNEL_URL) {
    console.error('❌ CLOUDFLARE_TUNNEL_URL is not set in your .env file.');
    console.log('   Open .env and set: CLOUDFLARE_TUNNEL_URL=https://your-tunnel.trycloudflare.com');
    process.exit(1);
  }

  console.log(`✅ Using tunnel URL from .env: ${TUNNEL_URL}`);

  const sketchPath = path.join(__dirname, 'sketch.ino');
  let content = fs.readFileSync(sketchPath, 'utf8');

  content = content.replace(
    /const char \*kioskApiUrl = ".*?";/g,
    `const char *kioskApiUrl = "${TUNNEL_URL}/api/hardware/event";`
  );
  content = content.replace(
    /const char \*kioskStatusUrl = ".*?";/g,
    `const char *kioskStatusUrl = "${TUNNEL_URL}/api/hardware/status";`
  );

  fs.writeFileSync(sketchPath, content);
  console.log('💾 sketch.ino firmware URLs updated!');
  console.log(`   kioskApiUrl    → ${TUNNEL_URL}/api/hardware/event`);
  console.log(`   kioskStatusUrl → ${TUNNEL_URL}/api/hardware/status`);
  console.log('\n👉 Next: rebuild firmware with: platformio run');
}

main().catch(err => {
  console.error('❌ Failed:', err.message);
});