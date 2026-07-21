require('dotenv').config();
const express = require('express');
const cors = require('cors');
const http = require('http');
const { Server } = require('socket.io');
const path = require('path');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const app = express();
const server = http.createServer(app);
// Read the Cloudflare tunnel URL from .env — update .env when the tunnel URL changes
const CLOUD_FLARE_TUNNEL_ORIGIN = process.env.CLOUDFLARE_TUNNEL_URL || '';
const allowedCorsOrigin = (origin, callback) => {
    if (!origin) return callback(null, true);

    const localNetworkPattern = /^https?:\/\/(localhost|127\.0\.0\.1|0\.0\.0\.0|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3})(:\d+)?$/;

    if (localNetworkPattern.test(origin) || origin === 'null' || origin === CLOUD_FLARE_TUNNEL_ORIGIN) {
        return callback(null, true);
    }

    return callback(new Error(`CORS blocked for origin: ${origin}`), false);
};
const io = new Server(server, { 
    cors: { 
        origin: allowedCorsOrigin,
        methods: ["GET", "POST"],
        allowedHeaders: ["Content-Type", "Authorization", "ngrok-skip-browser-warning"],
        credentials: true
    },
    pingTimeout: 60000,
    pingInterval: 25000,
    transports: ['websocket', 'polling']
});

const PORT = process.env.PORT || 3000;
const SERIAL_PORT_PATH = process.env.SERIAL_PORT || 'COM6';
const BAUD_RATE = parseInt(process.env.BAUD_RATE) || 115200;

// ============================================================
// ESP32 USB SERIAL BRIDGE
// Reads EVT:{...} JSON lines from the ESP32 over USB.
// Also writes SET_PRICE:<n> and RESET commands back to the ESP32.
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
        if (SERIAL_DEBUG) console.log(`[ESP32] ${line}`); // show all raw output in server terminal
        if (!line.startsWith('EVT:')) return; // plain debug log lines — ignore
        try {
            const data = JSON.parse(line.substring(4)); // strip "EVT:" prefix
            if (data.type === 'payment') {
                handleHardwarePayment(data);
            } else if (data.type === 'status') {
                handleHardwareStatus(data);
            }
        } catch (e) {
            console.warn(`[Serial] Could not parse event: ${line}`);
        }
    });

    esp32Port.on('error', (err) => {
        console.error(`[Serial] Port error: ${err.message}`);
    });

} catch (err) {
    console.warn(`[Serial] SerialPort init failed: ${err.message}`);
    console.warn('[Serial] Running without hardware — HTTP fallback only.');
}

// Send a command string to the ESP32 over serial (e.g. "SET_PRICE:150", "RESET")
function sendToESP32(command) {
    if (esp32Port && esp32Port.isOpen) {
        esp32Port.write(command + '\n', (err) => {
            if (err) console.error(`[Serial] Write error: ${err.message}`);
        });
    }
}

app.use(cors({
    origin: allowedCorsOrigin,
    methods: ["GET", "POST"],
    allowedHeaders: ["Content-Type", "Authorization", "ngrok-skip-browser-warning"],
    credentials: true
}));
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ============================================================
// MARITIME ROUTE CATALOG
// ============================================================
const ROUTES = [
    { id: 'R1', from: 'Manila', to: 'Cebu', price: 1250, duration: '22h' },
    { id: 'R2', from: 'Manila', to: 'Iloilo', price: 980, duration: '16h' },
    { id: 'R3', from: 'Cebu', to: 'Tagbilaran', price: 450, duration: '2h' },
    { id: 'R4', from: 'Batangas', to: 'Mindoro', price: 320, duration: '3h' },
    { id: 'R5', from: 'Dumaguete', to: 'Siquijor', price: 280, duration: '1.5h' }
];

// ============================================================
// GLOBAL KIOSK STATE
// ============================================================
let kioskState = {
    mode: 'IDLE',              // IDLE | BOOKING_ROUTE | BOOKING_PAYMENT | ONBOARD_SCAN | TICKET_ISSUED
    selectedRoute: null,
    passengerCount: 1,
    totalPrice: 0,
    credit: 0,
    changeDue: 0,
    qrData: null,
    receipt: null,
    lastHardwareEvent: null
};

// ============================================================
// RECEIPT GENERATOR (Thermal Printer Format)
// ============================================================
function generateReceipt(state) {
    const now = new Date();
    const dateStr = now.toLocaleDateString('en-PH', { year: 'numeric', month: 'short', day: 'numeric' });
    const timeStr = now.toLocaleTimeString('en-PH', { hour12: true });
    const ticketNo = `PKT-${Date.now().toString(36).toUpperCase()}`;
    
    const coins10 = Math.floor(state.changeDue / 10);
    const coins1 = state.changeDue % 10;

    const lines = [
        "================================",
        "     PORT KIOSK TESTER          ",
        "      OFFICIAL RECEIPT          ",
        "================================",
        ` Ticket #: ${ticketNo}`,
        ` Date: ${dateStr}`,
        ` Time: ${timeStr}`,
        "--------------------------------",
        " ROUTE DETAILS                  ",
        ` From: ${state.selectedRoute.from}`,
        ` To:   ${state.selectedRoute.to}`,
        ` Duration: ${state.selectedRoute.duration}`,
        "--------------------------------",
        ` PASSENGERS: ${state.passengerCount}`,
        ` UNIT PRICE: ₱${state.selectedRoute.price.toLocaleString()}`,
        "--------------------------------",
        ` TOTAL AMOUNT: ₱${state.totalPrice.toLocaleString()}`,
        ` CASH TENDERED: ₱${state.credit.toLocaleString()}`,
        ` CHANGE GIVEN: ₱${state.changeDue.toLocaleString()}`,
        "--------------------------------",
        " CHANGE BREAKDOWN               ",
        `   ₱10 coins: ${coins10}`,
        `   ₱1 coins:  ${coins1}`,
        "================================",
        " PLEASE KEEP THIS RECEIPT       ",
        " FOR BOARDING VERIFICATION      ",
        "                                ",
        " THANK YOU FOR TRAVELING WITH   ",
        "            US!                 ",
        "================================"
    ];

    return {
        text: lines.join('\n'),
        ticketNo,
        qrData: `BOARD:${ticketNo}:${state.selectedRoute.id}:${state.passengerCount}`,
        printable: true
    };
}

// ============================================================
// HARDWARE EVENT HANDLERS (shared by Serial and HTTP paths)
// ============================================================

function handleHardwarePayment({ device, amount, credit, timestamp }) {
    if (kioskState.mode === 'BOOKING_PAYMENT') {
        kioskState.credit = credit || (kioskState.credit + amount);
        kioskState.lastHardwareEvent = { device, amount, timestamp };

        io.emit('payment-update', {
            device,
            amount,
            credit: kioskState.credit,
            remaining: Math.max(0, kioskState.totalPrice - kioskState.credit),
            timestamp
        });

        if (kioskState.credit >= kioskState.totalPrice) {
            finalizePayment();
        }
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
        kioskState.receipt = generateReceipt(kioskState);
        kioskState.mode = 'TICKET_ISSUED';
        io.emit('state-update', kioskState);
        setTimeout(() => resetKiosk(), 8000);
    } else if (status === 'insufficient_funds') {
        io.emit('status-message', { type: 'warning', text: 'Insufficient funds. Please add more payment.' });
    }
}

// ============================================================
// HARDWARE ENDPOINTS (HTTP fallback — ESP32 can still POST here)
// ============================================================

app.post('/api/hardware/event', (req, res) => {
    const { device, amount, credit, timestamp } = req.body;
    handleHardwarePayment({ device, amount, credit, timestamp });
    res.json({ status: "ok", mode: kioskState.mode, credit: kioskState.credit });
});

app.post('/api/hardware/status', (req, res) => {
    const { status, changeDue } = req.body;
    handleHardwareStatus({ status, changeDue });
    res.json({ status: "ok" });
});

// ============================================================
// FRONTEND API ENDPOINTS
// ============================================================

// Returns config values the frontend needs (e.g. tunnel URL for socket.io)
app.get('/api/config', (req, res) => {
    res.json({
        tunnelUrl: CLOUD_FLARE_TUNNEL_ORIGIN || null
    });
});

app.get('/api/routes', (req, res) => res.json(ROUTES));

app.post('/api/action/select-mode', (req, res) => {
    const { mode } = req.body;
    resetKiosk();
    kioskState.mode = mode === 'BOOK' ? 'BOOKING_ROUTE' : 'ONBOARD_SCAN';
    io.emit('state-update', kioskState);
    res.json(kioskState);
});

app.post('/api/action/select-route', (req, res) => {
    const { routeId, passengers } = req.body;
    const route = ROUTES.find(r => r.id === routeId);
    
    if (!route) return res.status(404).json({ error: 'Route not found' });

    kioskState.selectedRoute = route;
    kioskState.passengerCount = Math.max(1, Math.min(10, passengers || 1));
    kioskState.totalPrice = route.price * kioskState.passengerCount;
    kioskState.mode = 'BOOKING_PAYMENT';
    kioskState.credit = 0;
    kioskState.changeDue = 0;

    // Tell the ESP32 the target price so it knows when to dispense change
    sendToESP32(`SET_PRICE:${kioskState.totalPrice}`);
    
    io.emit('state-update', kioskState);
    res.json(kioskState);
});

app.post('/api/action/simulate-scan', (req, res) => {
    kioskState.qrData = `TICKET-MNL-CEB-${Date.now()}-USR${Math.floor(Math.random()*999)}`;
    kioskState.receipt = {
        text: `================================\n     BOARDING PASS VERIFIED     \n================================\n Ticket: ${kioskState.qrData}\n Status: VALID ✅\n Route: Manila → Cebu\n Passengers: 1\n================================\n WELCOME ABOARD!\n================================`,
        ticketNo: kioskState.qrData,
        qrData: kioskState.qrData,
        printable: true
    };
    kioskState.mode = 'TICKET_ISSUED';
    io.emit('state-update', kioskState);
    setTimeout(() => resetKiosk(), 6000);
    res.json({ success: true, ticket: kioskState.qrData });
});

app.post('/api/action/reset', (req, res) => {
    resetKiosk();
    io.emit('state-update', kioskState);
    res.json({ status: "reset" });
});

// ============================================================
// HELPERS
// ============================================================

function finalizePayment() {
    kioskState.changeDue = kioskState.credit - kioskState.totalPrice;
    kioskState.mode = 'DISPENSING_CHANGE';
    io.emit('state-update', kioskState);
    
    // Only auto-complete if no ESP32 hardware is connected.
    // When hardware is present, the ESP32 will send a 'completed' status event
    // after dispensing change — avoid duplicate receipt/reset.
    if (!esp32Port || !esp32Port.isOpen) {
        setTimeout(() => {
            if (kioskState.mode === 'DISPENSING_CHANGE') {
                kioskState.receipt = generateReceipt(kioskState);
                kioskState.mode = 'TICKET_ISSUED';
                io.emit('state-update', kioskState);
                setTimeout(() => resetKiosk(), 8000);
            }
        }, 4000);
    }
}

function resetKiosk() {
    kioskState = {
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
    sendToESP32('RESET'); // sync ESP32 state back to IDLE
}

// ============================================================
// WEBSOCKET CONNECTION
// ============================================================
io.on('connection', (socket) => {
    console.log(`[WS] Frontend connected: ${socket.id}`);
    socket.emit('routes-data', ROUTES);
    socket.emit('state-update', kioskState);
    
    socket.on('disconnect', () => {
        console.log(`[WS] Frontend disconnected: ${socket.id}`);
    });
});

// ============================================================
// START SERVER
// ============================================================
server.listen(PORT, () => {
    console.log(`\n🚢 Port Kiosk Tester Server Active`);
    console.log(`   ️  Frontend: http://localhost:${PORT}`);
    console.log(`   🔌 ESP32 POST → /api/hardware/event`);
    console.log(`   🔌 ESP32 POST → /api/hardware/status`);
    console.log(`   📡 WebSocket: Real-time sync enabled\n`);
});