const fs = require('fs');
const path = require('path');
const http = require('http');

function getNgrokUrl() {
  return new Promise((resolve, reject) => {
    http.get('http://127.0.0.1:4040/api/tunnels', (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => {
        try {
          const json = JSON.parse(data);
          const tunnel = json.tunnels.find(t => t.proto === 'https');
          if (!tunnel) return reject(new Error('No https tunnel found yet'));
          resolve(tunnel.public_url);
        } catch (e) { reject(e); }
      });
    }).on('error', reject);
  });
}

async function main() {
  console.log('🔍 Checking ngrok API for tunnel URL...');
  const TUNNEL_URL = await getNgrokUrl();
  console.log(`✅ Detected Tunnel: ${TUNNEL_URL}`);

  const sketchPath = path.join(__dirname, 'sketch.ino');
  let content = fs.readFileSync(sketchPath, 'utf8');
  content = content.replace(/const char\* kioskApiUrl = ".*?";/g, `const char* kioskApiUrl = "${TUNNEL_URL}/api/hardware/event";`);
  content = content.replace(/const char\* kioskStatusUrl = ".*?";/g, `const char* kioskStatusUrl = "${TUNNEL_URL}/api/hardware/status";`);
  fs.writeFileSync(sketchPath, content);
  console.log('💾 Firmware URLs updated!');
}

main().catch(err => {
  console.error('❌ Failed:', err.message);
  console.log('⚠️  Make sure `ngrok http 3000` is already running (check http://127.0.0.1:4040) before running this script.');
});