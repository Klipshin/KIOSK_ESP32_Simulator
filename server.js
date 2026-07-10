const express = require('express');
const cors = require('cors');
const http = require('http');
const { Server } = require('socket.io');
const path = require('path');

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
        allowedHeaders: ["Content-Type", "Authorization", "ngrok-skip-browser-warning"],
        credentials: true
    },
    pingTimeout: 60000,
    pingInterval: 25000,
    transports: ['websocket', 'polling']
});

const PORT = process.env.PORT || 3000;

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
// HARDWARE ENDPOINTS (ESP32 → Server)
// ============================================================

app.post('/api/hardware/event', (req, res) => {
    const { device, amount, credit, timestamp } = req.body;
    
    // Only accept money during active payment phase
    if (kioskState.mode === 'BOOKING_PAYMENT') {
        kioskState.credit = credit || (kioskState.credit + amount);
        kioskState.lastHardwareEvent = { device, amount, timestamp };
        
        // Broadcast real-time update to frontend
        io.emit('payment-update', {
            device,
            amount,
            credit: kioskState.credit,
            remaining: Math.max(0, kioskState.totalPrice - kioskState.credit),
            timestamp
        });

        // Auto-trigger completion when fully paid
        if (kioskState.credit >= kioskState.totalPrice) {
            finalizePayment();
        }
    } else {
        console.log(`[HW] Ignored ${device} ₱${amount} (Mode: ${kioskState.mode})`);
    }
    
    res.json({ status: "ok", mode: kioskState.mode, credit: kioskState.credit });
});

app.post('/api/hardware/status', (req, res) => {
    const { status, changeDue, timestamp } = req.body;
    
    kioskState.changeDue = changeDue || 0;
    
    if (status === 'dispensing_change') {
        io.emit('dispense-change', { 
            amount: kioskState.changeDue,
            coins10: Math.floor(kioskState.changeDue / 10),
            coins1: kioskState.changeDue % 10
        });
    } else if (status === 'completed') {
        // Hardware finished dispensing → issue receipt
        kioskState.receipt = generateReceipt(kioskState);
        kioskState.mode = 'TICKET_ISSUED';
        io.emit('state-update', kioskState);
        
        // Auto-reset after showing receipt
        setTimeout(() => resetKiosk(), 8000);
    } else if (status === 'insufficient_funds') {
        io.emit('status-message', { type: 'warning', text: 'Insufficient funds. Please add more payment.' });
    }
    
    res.json({ status: "ok" });
});

// ============================================================
// FRONTEND API ENDPOINTS
// ============================================================

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
    
    // In real hardware, ESP32 would dispense then POST /api/hardware/status
    // For simulation without hardware, auto-complete after delay:
    setTimeout(() => {
        if (kioskState.mode === 'DISPENSING_CHANGE') {
            kioskState.receipt = generateReceipt(kioskState);
            kioskState.mode = 'TICKET_ISSUED';
            io.emit('state-update', kioskState);
            setTimeout(() => resetKiosk(), 8000);
        }
    }, 4000);
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