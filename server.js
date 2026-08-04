require('dotenv').config();
const express = require('express');
const cors = require('cors');
const http = require('http');
const { Server } = require('socket.io');
const path = require('path');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const axios = require('axios');

// ============================================================
// CONFIGURATION & API CLIENT
// ============================================================
const PORT = process.env.PORT || 3000;
const SERIAL_PORT_PATH = process.env.SERIAL_PORT || 'COM6';
const BAUD_RATE = parseInt(process.env.BAUD_RATE) || 115200;

const V2_API_BASE = process.env.V2_API_URL || 'http://localhost:4000/api/v2';
const KIOSK_SERVICE_KEY = process.env.KIOSK_SERVICE_KEY || '';
const TERMINAL_ID = process.env.TERMINAL_ID || 'TERM-DEV-001';
const SHIPPING_LINE_FILTER = process.env.SHIPPING_LINE_FILTER || null;

const apiClient = axios.create({
  baseURL: V2_API_BASE,
  headers: {
    'X-Terminal-Key': KIOSK_SERVICE_KEY,
    'X-Terminal-Id': TERMINAL_ID,
    'Content-Type': 'application/json'
  },
  timeout: 10000
});

// Add this after apiClient creation
const MOCK_MODE = process.env.MOCK_MODE === 'true';

// Override startKioskSession to use mock data when V2 is unavailable
async function startKioskSession() {
  if (MOCK_MODE || !V2_API_BASE) {
    console.warn('[V2] Running in MOCK MODE — using local fallback routes');
    tenantRoutes = [
      { id: 'R1', from: 'Manila', to: 'Cebu', price: 1250, duration: '22h' },
      { id: 'R2', from: 'Cebu', to: 'Tagbilaran', price: 450, duration: '2h' }
    ];
    currentSessionId = `mock-session-${Date.now()}`;
    io.emit('routes-data', tenantRoutes);
    return;
  }

  try {
    const res = await apiClient.post('/kiosk/sessions', {
      terminalId: TERMINAL_ID,
      shippingLineFilter: SHIPPING_LINE_FILTER
    });
    currentSessionId = res.data.sessionId;
    tenantRoutes = res.data.routes;
    io.emit('routes-data', tenantRoutes);
    console.log(`[V2] Session started: ${currentSessionId}, ${tenantRoutes.length} routes loaded`);
  } catch (err) {
    console.error('[V2] Failed to start session:', err.response?.data || err.message);
    io.emit('status-message', { 
      type: 'error', 
      text: 'Cannot connect to booking server. Please contact staff.' 
    });
  }
}

// ============================================================
// EXPRESS & SOCKET.IO SETUP
// ============================================================
const app = express();
const server = http.createServer(app);

const allowedCorsOrigin = (origin, callback) => {
  if (!origin) return callback(null, true);
  const localNetworkPattern = /^https?:\/\/(localhost|127\.0\.0\.1|0\.0\.0\.0|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3})(:\d+)?$/;
  
  if (localNetworkPattern.test(origin) || origin === 'null') {
    return callback(null, true);
  }
  return callback(new Error(`CORS blocked for origin: ${origin}`), false);
};

const io = new Server(server, { 
  cors: { 
    origin: allowedCorsOrigin,
    methods: ["GET", "POST"],
    allowedHeaders: ["Content-Type", "Authorization"],
    credentials: true
  },
  pingTimeout: 60000,
  pingInterval: 25000,
  transports: ['websocket', 'polling']
});

app.use(cors({ origin: allowedCorsOrigin, credentials: true }));
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ============================================================
// ESP32 USB SERIAL BRIDGE
// ============================================================
let esp32Port = null;
try {
  esp32Port = new SerialPort({ path: SERIAL_PORT_PATH, baudRate: BAUD_RATE, autoOpen: false });
  esp32Port.open((err) => {
    if (err) {
      console.warn(`[Serial] Could not open ${SERIAL_PORT_PATH}: ${err.message}`);
      console.warn('[Serial] Hardware events will only arrive via HTTP fallback endpoints.');
      return;
    }
    console.log(`[Serial] Opened ${SERIAL_PORT_PATH} at ${BAUD_RATE} baud — ESP32 connected.`);
  });

  const parser = esp32Port.pipe(new ReadlineParser({ delimiter: '\n' }));
  const SERIAL_DEBUG = process.env.SERIAL_DEBUG === 'true';

  parser.on('data', (line) => {
    line = line.trim();
    if (SERIAL_DEBUG) {
      const cleanLine = line.replace(/[^ -~]/g, '');
      if (cleanLine.length > 0) console.log(`[ESP32] ${cleanLine}`);
    }
    if (!line.startsWith('EVT:')) return;
    try {
      const data = JSON.parse(line.substring(4));
      if (data.type === 'payment') handleHardwarePayment(data);
      else if (data.type === 'status') handleHardwareStatus(data);
    } catch (e) {
      console.warn(`[Serial] Could not parse event: ${line}`);
    }
  });

  esp32Port.on('error', (err) => console.error(`[Serial] Port error: ${err.message}`));
} catch (err) {
  console.warn(`[Serial] SerialPort init failed: ${err.message}`);
  console.warn('[Serial] Running without hardware — HTTP fallback only.');
}

function sendToESP32(command) {
  if (esp32Port && esp32Port.isOpen) {
    esp32Port.write(command + '\n', (err) => {
      if (err) console.error(`[Serial] Write error: ${err.message}`);
    });
  }
}

// ============================================================
// GLOBAL KIOSK STATE
// ============================================================
let kioskState = {
  mode: 'IDLE',
  selectedRoute: null,
  passengerCount: 1,
  totalPrice: 0,
  credit: 0,
  changeDue: 0,
  qrData: null,
  receipt: null,
  lastHardwareEvent: null
};

let currentSessionId = null;
let tenantRoutes = [];

// ============================================================
// V2 API INTEGRATION
// ============================================================


// Start session on boot and refresh every 5 minutes
startKioskSession();
setInterval(startKioskSession, 300000);

// ============================================================
// HARDWARE EVENT HANDLERS
// ============================================================
function handleHardwarePayment({ device, amount, credit, timestamp }) {
  if (kioskState.mode === 'BOOKING_PAYMENT') {
    kioskState.credit = credit || (kioskState.credit + amount);
    kioskState.lastHardwareEvent = { device, amount, timestamp };

    // Sync payment progress with V2 API
    apiClient.patch(`/kiosk/bookings/${currentSessionId}/payment-progress`, {
      sessionId: currentSessionId,
      amountInserted: kioskState.credit,
      totalDue: kioskState.totalPrice,
      idempotencyKey: `PAY-${Date.now()}-${kioskState.credit}`
    }).catch(err => console.warn('[V2] Payment sync failed:', err.message));

    io.emit('payment-update', {
      device, amount, credit: kioskState.credit,
      remaining: Math.max(0, kioskState.totalPrice - kioskState.credit),
      timestamp
    });
  } else {
    console.log(`[HW] Ignored ${device} ₱${amount} (Mode: ${kioskState.mode})`);
  }
}

function handleHardwareStatus({ status, changeDue }) {
  kioskState.changeDue = changeDue || 0;

  if (status === 'dispensing_change') {
    io.emit('dispense-change', {
      amount: kioskState.changeDue,
      coins10: Math.floor(kioskState.changeDue / 10),
      coins1: kioskState.changeDue % 10
    });
  } else if (status === 'completed') {
    kioskState.mode = 'TICKET_ISSUED';
    io.emit('state-update', kioskState);
    setTimeout(() => resetKiosk(), 8000);
  } else if (status === 'insufficient_funds') {
    io.emit('status-message', { type: 'warning', text: 'Insufficient funds. Please add more payment.' });
  } else if (status === 'hardware_error_jam') {
    io.emit('status-message', { type: 'error', text: 'Hardware Error: Hopper Jam detected! Please call assistance.' });
    setTimeout(() => resetKiosk(), 8000);
  }
}

// ============================================================
// FRONTEND API ENDPOINTS
// ============================================================
app.get('/api/routes', (req, res) => res.json(tenantRoutes));

app.post('/api/action/select-mode', (req, res) => {
  const { mode } = req.body;
  resetKiosk();
  kioskState.mode = mode === 'BOOK' ? 'BOOKING_ROUTE' : 'ONBOARD_SCAN';
  io.emit('state-update', kioskState);
  res.json(kioskState);
});

app.post('/api/action/select-route', async (req, res) => {
  const { routeId, passengers } = req.body;
  const route = tenantRoutes.find(r => r.id === routeId);
  
  if (!route) return res.status(404).json({ error: 'Route not found' });

  try {
    // Hold inventory in V2 API before accepting payment
    const holdRes = await apiClient.post('/kiosk/hold-inventory', {
      sessionId: currentSessionId,
      routeId: route.id,
      passengers: Math.max(1, Math.min(10, passengers || 1))
    });

    kioskState.selectedRoute = route;
    kioskState.passengerCount = holdRes.data.passengerCount;
    kioskState.totalPrice = holdRes.data.fareTotal;
    kioskState.mode = 'BOOKING_PAYMENT';
    kioskState.credit = 0;
    kioskState.changeDue = 0;

    sendToESP32(`SET_PRICE:${kioskState.totalPrice}`);
    io.emit('state-update', kioskState);
    res.json(kioskState);
  } catch (err) {
    console.error('[V2] Inventory hold failed:', err.response?.data || err.message);
    res.status(500).json({ error: 'Failed to reserve seats. Please try again.' });
  }
});

app.post('/api/action/pay', async (req, res) => {
  if (kioskState.mode !== 'BOOKING_PAYMENT') {
    return res.status(400).json({ error: 'Not in booking payment mode' });
  }
  if (kioskState.credit < kioskState.totalPrice) {
    return res.status(400).json({ error: 'Insufficient credit' });
  }
  
  await finalizePayment();
  res.json(kioskState);
});

app.post('/api/action/reset', (req, res) => {
  resetKiosk();
  io.emit('state-update', kioskState);
  res.json({ status: "reset" });
});

// ============================================================
// HELPERS
// ============================================================
async function finalizePayment() {
  kioskState.changeDue = kioskState.credit - kioskState.totalPrice;
  kioskState.mode = 'DISPENSING_CHANGE';
  io.emit('state-update', kioskState);
  
  if (esp32Port && esp32Port.isOpen) {
    sendToESP32(`DISPENSE:${kioskState.changeDue}`);
  }

  // ✅ Mock booking when V2 is unavailable
  if (MOCK_MODE) {
    setTimeout(() => {
      kioskState.receipt = { url: '/mock-receipt.pdf', bookingId: `MOCK-${Date.now()}` };
      kioskState.bookingId = kioskState.receipt.bookingId;
      kioskState.qrData = `MOCK-QR-${Date.now()}`;
      kioskState.mode = 'TICKET_ISSUED';
      io.emit('state-update', kioskState);
      console.log('[MOCK] Booking simulated successfully');
      setTimeout(() => resetKiosk(), 8000);
    }, 1500);
    return;
  }

  // Real V2 booking logic below...
  try {
    const bookingRes = await apiClient.post('/kiosk/bookings', {
      sessionId: currentSessionId,
      routeId: kioskState.selectedRoute.id,
      passengerCount: kioskState.passengerCount,
      cashAmount: kioskState.credit,
      changeGiven: kioskState.changeDue,
      idempotencyKey: `PAY-${Date.now()}-${kioskState.credit}`
    });

    kioskState.receipt = bookingRes.data.receipt;
    kioskState.bookingId = bookingRes.data.bookingId;
    kioskState.qrData = bookingRes.data.qrCodeData;
    kioskState.mode = 'TICKET_ISSUED';
    io.emit('state-update', kioskState);
    
    console.log(`[V2] Booking created: ${kioskState.bookingId}`);
  } catch (err) {
    console.error('[V2] Booking failed:', err.response?.data || err.message);
    io.emit('status-message', { 
      type: 'error', 
      text: 'Booking failed. Cash recorded but ticket not issued. Contact staff.' 
    });
    // Do NOT auto-reset — let staff intervene for failed bookings
  }
}

function resetKiosk() {
  kioskState = {
    mode: 'IDLE', selectedRoute: null, passengerCount: 1,
    totalPrice: 0, credit: 0, changeDue: 0,
    qrData: null, receipt: null, lastHardwareEvent: null
  };
  sendToESP32('RESET');
}

// ============================================================
// WEBSOCKET CONNECTION
// ============================================================
io.on('connection', (socket) => {
  console.log(`[WS] Frontend connected: ${socket.id}`);
  socket.emit('routes-data', tenantRoutes);
  socket.emit('state-update', kioskState);
  
  socket.on('disconnect', () => console.log(`[WS] Frontend disconnected: ${socket.id}`));
});

// ============================================================
// START SERVER
// ============================================================
server.listen(PORT, () => {
  console.log(`\n🚢 Port Kiosk Bridge Server Active`);
  console.log(`   ️  Frontend: http://localhost:${PORT}`);
  console.log(`   🔌 ESP32: ${SERIAL_PORT_PATH} @ ${BAUD_RATE} baud`);
  console.log(`   ☁️  V2 API: ${V2_API_BASE}`);
  console.log(`   🆔 Terminal: ${TERMINAL_ID}`);
  console.log(`   📡 WebSocket: Real-time sync enabled\n`);
});